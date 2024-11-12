#ifndef GLOBAL_SPACE_H
#define GLOBAL_SPACE_H

#include <algorithm>
#include <deque>
#include <forward_list>
#include <map>
#include <unordered_map>
#include <vector>

#include <cassert>
#include <iostream>
#include <ostream>

#include "config.h"

#ifdef CONFIG_PARALLEL
#include "tbb/concurrent_hash_map.h"
#include "tbb/enumerable_thread_specific.h"
#include "tbb/parallel_for.h"
#include <atomic>
#endif

#include "problem.hpp"
#include "global/state_space_data.hpp"
#include "clock.hpp"

#include "global/state.hpp"

namespace NP {

	namespace Global {

		template<class Time> class State_space
		{
		public:

			typedef Scheduling_problem<Time> Problem;
			typedef typename Scheduling_problem<Time>::Workload Workload;
			typedef typename Scheduling_problem<Time>::Precedence_constraints Precedence_constraints;
			typedef typename Scheduling_problem<Time>::Abort_actions Abort_actions;
			typedef Schedule_state<Time> State;
			typedef typename std::vector<Interval<Time>> CoreAvailability;

			typedef Schedule_node<Time> Node;

			static State_space* explore(
				const Problem& prob,
				const Analysis_options& opts)
			{
				if (opts.verbose)
					std::cout << "Starting" << std::endl;

				State_space* s = new State_space(prob.jobs, prob.prec, prob.aborts, prob.num_processors, 
					{ opts.merge_conservative, opts.merge_use_job_finish_times, opts.merge_depth }, opts.timeout, opts.max_depth, opts.early_exit, opts.verbose);
				s->be_naive = opts.be_naive;
				if (opts.verbose)
					std::cout << "Analysing" << std::endl;
				s->cpu_time.start();
				s->explore();
				s->cpu_time.stop();
				return s;
			}

			// convenience interface for tests
			static State_space* explore_naively(
				const Workload& jobs,
				unsigned int num_cpus = 1)
			{
				Problem p{ jobs, num_cpus };
				Analysis_options o;
				o.be_naive = true;
				return explore(p, o);
			}

			// convenience interface for tests
			static State_space* explore(
				const Workload& jobs,
				unsigned int num_cpus = 1)
			{
				Problem p{ jobs, num_cpus };
				Analysis_options o;
				return explore(p, o);
			}

			// return the BCRT and WCRT of job j 
			Interval<Time> get_finish_times(const Job<Time>& j) const
			{
				return get_finish_times(j.get_job_index());
			}

			Interval<Time> get_finish_times(Job_index j) const
			{
				if (rta[j].valid) {
					return rta[j].rt;
				}
				else {
					return Interval<Time>{0, Time_model::constants<Time>::infinity()};
				}
			}

			bool is_schedulable() const
			{
				return !aborted && !observed_deadline_miss;
			}

			bool was_timed_out() const
			{
				return timed_out;
			}

			//currently unused, only required to compile nptest.cpp correctly
			unsigned long number_of_nodes() const
			{
				return num_nodes;
			}

			unsigned long number_of_states() const
			{
				return num_states;
			}

			unsigned long number_of_edges() const
			{
				return num_edges;
			}

			unsigned long max_exploration_front_width() const
			{
				return max_width;
			}

			const std::vector<std::pair<unsigned long, unsigned long>>& evolution_exploration_front_width() const
			{
				return width;
			}

			double get_cpu_time() const
			{
				return cpu_time;
			}

			typedef std::deque<Node> Nodes;
			typedef std::deque<State> States;

#ifdef CONFIG_PARALLEL
			typedef tbb::enumerable_thread_specific< Nodes > Split_nodes;
			typedef std::deque<Split_nodes> Nodes_storage;
#else
			typedef std::deque< Nodes > Nodes_storage;
#endif

#ifdef CONFIG_COLLECT_SCHEDULE_GRAPH

			struct Edge {
				const Job<Time>* scheduled;
				const Node* source;
				const Node* target;
				const Interval<Time> finish_range;
				const unsigned int parallelism;

				Edge(const Job<Time>* s, const Node* src, const Node* tgt,
					const Interval<Time>& fr, unsigned int parallelism = 1)
					: scheduled(s)
					, source(src)
					, target(tgt)
					, finish_range(fr)
					, parallelism(parallelism)
				{
				}

				bool deadline_miss_possible() const
				{
					return scheduled->exceeds_deadline(finish_range.upto());
				}

				Time earliest_finish_time() const
				{
					return finish_range.from();
				}

				Time latest_finish_time() const
				{
					return finish_range.upto();
				}

				Time earliest_start_time() const
				{
					return finish_range.from() - scheduled->least_exec_time();
				}

				Time latest_start_time() const
				{
					return finish_range.upto() - scheduled->maximal_exec_time();
				}

				unsigned int parallelism_level() const
				{
					return parallelism;
				}
			};

			const std::deque<Edge>& get_edges() const
			{
				return edges;
			}

			const Nodes_storage& get_nodes() const
			{
				return nodes_storage;
			}


#endif
		private:

			typedef Node* Node_ref;
			typedef typename std::forward_list<Node_ref> Node_refs;
			typedef State* State_ref;
			typedef typename std::forward_list<State_ref> State_refs;

#ifdef CONFIG_PARALLEL
			typedef tbb::concurrent_hash_map<hash_value_t, Node_refs> Nodes_map;
			typedef typename Nodes_map::accessor Nodes_map_accessor;
#else
			typedef std::unordered_map<hash_value_t, Node_refs> Nodes_map;
#endif
			typedef const Job<Time>* Job_ref;

			// Similar to uni/space.hpp, make Response_times a vector of intervals.

			// typedef std::unordered_map<Job_index, Interval<Time> > Response_times;
			struct Response_time_item {
				bool valid;
				Interval<Time> rt;
				Interval<Time> n_rt;

				Response_time_item()
					: valid(false)
					, rt(0, 0)
					, n_rt(0, 0)
				{
				}
			};
			typedef std::vector<Response_time_item> Response_times;
			//typedef std::vector<Time> r_bar_min;

#ifdef CONFIG_COLLECT_SCHEDULE_GRAPH
			std::deque<Edge> edges;
#endif
			// Similar to uni/space.hpp, make rta a 

			Response_times rta;

#ifdef CONFIG_PARALLEL
			tbb::enumerable_thread_specific<Response_times> partial_rta;
#endif
			bool verbose;
			bool aborted;
			bool timed_out;
			bool observed_deadline_miss;
			bool early_exit;

			const unsigned int max_depth;

			bool be_naive;

			struct Merge_options {
				bool conservative; 
				bool use_finish_times; 
				int budget;
			};
			const Merge_options merge_opts;
			Nodes_storage nodes_storage;
			Nodes_map nodes_by_key;

#ifdef CONFIG_PARALLEL
			std::atomic_ulong num_nodes, num_states, num_edges;
#else
			unsigned long num_nodes, num_states, num_edges;
#endif
			// updated only by main thread
			unsigned long current_job_count, max_width;
			std::vector<std::pair<unsigned long, unsigned long>> width;

#ifdef CONFIG_PARALLEL
			tbb::enumerable_thread_specific<unsigned long> edge_counter;
#endif
			Processor_clock cpu_time;
			const double timeout;
			const unsigned int num_cpus;

			State_space_data<Time> state_space_data;

			State_space(const Workload& jobs,
				const Precedence_constraints& edges,
				const Abort_actions& aborts,
				unsigned int num_cpus,
				Merge_options merge_options,
				double max_cpu_time = 0,
				unsigned int max_depth = 0,
				bool early_exit = true,
				bool verbose = false)
				: state_space_data(jobs, edges, aborts, num_cpus)
				, aborted(false)
				, timed_out(false)
				, observed_deadline_miss(false)
				, be_naive(false)		
				, timeout(max_cpu_time)
				, max_depth(max_depth)
				, merge_opts(merge_options)
				, verbose(verbose)
				, num_nodes(0)
				, num_states(0)
				, num_edges(0)
				, max_width(0)
				, width(jobs.size(), { 0,0 })
				, rta(jobs.size())
				, current_job_count(0)
				, num_cpus(num_cpus)
				, early_exit(early_exit)
#ifdef CONFIG_PARALLEL
				, partial_rta(jobs.size())
#endif
			{
				//r_bar_min r_bar_mins;
				//r_bar_mins.resize(jobs.size());
			}

		private:

			void count_edge()
			{
#ifdef CONFIG_PARALLEL
				edge_counter.local()++;
#else
				num_edges++;
#endif
			}

			void update_finish_times(Response_times& r, const Job_index id,
				Interval<Time> range, Time n_bcrt, Time n_wcrt)
			{
				if (!r[id].valid) {
					r[id].valid = true;
					r[id].rt = range;
					r[id].n_rt = Interval(n_bcrt, n_wcrt);
				}
				else {
					r[id].rt |= range;
				}
				DM("RTA " << id << ": " << r[id].rt << std::endl);
				DM("N_RTA " << id << ": " << r[id].n_rt << std::endl);
				//DM("In  A: " << state_space_data.predecessors[id] << std::endl);
			}

			void update_finish_times(
				Response_times& r, const Job<Time>& j, Interval<Time> range, Time n_bcrt, Time n_wcrt)
			{
				update_finish_times(r, j.get_job_index(), range, n_bcrt, n_wcrt);
				if (j.exceeds_deadline(range.upto())) {
					observed_deadline_miss = true;

					if (early_exit)
						aborted = true;
				}
			}

			void update_finish_times(const Job<Time>& j, Interval<Time> range, Time n_bcrt, Time n_wcrt)
			{
				Response_times& r =
#ifdef CONFIG_PARALLEL
					partial_rta.local();
#else
					rta;
#endif
				update_finish_times(r, j, range, n_bcrt, n_wcrt);
			}

			

			void make_initial_node(unsigned num_cores)
			{
				// construct initial state
				nodes_storage.emplace_back();
				Node& n = new_node(num_cores, state_space_data);
				State& s = new_state(num_cores, state_space_data);
				n.add_state(&s);
				num_states++;
			}

			Nodes& nodes()
			{
#ifdef CONFIG_PARALLEL
				return nodes_storage.back().local();
#else
				return nodes_storage.back();
#endif
			}

			template <typename... Args>
			Node_ref alloc_node(Args&&... args)
			{
				nodes().emplace_back(std::forward<Args>(args)...);
				Node_ref n = &(*(--nodes().end()));

				// make sure we didn't screw up...
				auto njobs = n->number_of_scheduled_jobs();
				assert(
					(!njobs && num_states == 0) // initial state
					|| (njobs == current_job_count + 1) // normal State
					|| (njobs == current_job_count + 2 && aborted) // deadline miss
				);

				return n;
			}

			template <typename... Args>
			State& new_state(Args&&... args)
			{
				return *(new State(std::forward<Args>(args)...));
			}


			template <typename... Args>
			void new_or_merge_state(Node& n, Args&&... args)
			{
				// create a new state.
				State& new_s = new_state(std::forward<Args>(args)...);

				// try to merge the new state with existing states in node n.
				if (!(n.get_states()->empty())) {
					int n_states_merged = n.merge_states(new_s, merge_opts.conservative, merge_opts.use_finish_times, merge_opts.budget);
					if (n_states_merged > 0) {
						delete& new_s; // if we could merge no need to keep track of the new state anymore
						num_states -= (n_states_merged - 1);
					}
					else
					{
						n.add_state(&new_s); // else add the new state to the node
						num_states++;
					}
				}
				else
				{
					n.add_state(&new_s); // else add the new state to the node
					num_states++;
				}
			}


#ifdef CONFIG_PARALLEL
			// make node available for fast lookup
			void insert_cache_node(Nodes_map_accessor& acc, Node_ref n)
			{
				assert(!acc.empty());

				Node_refs& list = acc->second;
				list.push_front(n);
			}

			template <typename... Args>
			Node& new_node_at(Nodes_map_accessor& acc, Args&&... args)
			{
				assert(!acc.empty());
				Node_ref n = alloc_node(std::forward<Args>(args)...);
				DM("new node - global " << n << std::endl);
				// add node to nodes_by_key map.
				insert_cache_node(acc, n);
				num_nodes++;
				return *n;
			}

			template <typename... Args>
			Node& new_node(Args&&... args)
			{
				Nodes_map_accessor acc;
				Node_ref n = alloc_node(std::forward<Args>(args)...);
				while (true) {
					if (nodes_by_key.find(acc, n->get_key()) || nodes_by_key.insert(acc, n->get_key())) {
						DM("new node - global " << n << std::endl);
						// add node to nodes_by_key map.
						insert_cache_node(acc, n);
						num_nodes++;
						return *n;
					}
				}
			}

#else
			void cache_node(Node_ref n)
			{
				// create a new list if needed, or lookup if already existing
				auto res = nodes_by_key.emplace(
					std::make_pair(n->get_key(), Node_refs()));

				auto pair_it = res.first;
				Node_refs& list = pair_it->second;

				list.push_front(n);
			}

			template <typename... Args>
			Node& new_node(Args&&... args)
			{
				Node_ref n = alloc_node(std::forward<Args>(args)...);
				DM("This is called" << std::endl);
				DM("new node - global " << n << std::endl);
				// add node to nodes_by_key map.
				cache_node(n);
				num_nodes++;
				return *n;
			}
#endif

			void check_cpu_timeout()
			{
				if (timeout && get_cpu_time() > timeout) {
					aborted = true;
					timed_out = true;
				}
			}

			void check_depth_abort()
			{
				if (max_depth && current_job_count > max_depth)
					aborted = true;
			}

			bool unfinished(const Node& n, const Job<Time>& j) const
			{
				return n.job_incomplete(j.get_job_index());
			}

			// Check if any job is guaranteed to miss its deadline in any state in node new_n
			void check_for_deadline_misses(const Node& old_n, const Node& new_n)
			{
				auto check_from = old_n.get_first_state()->core_availability().min();

				// check if we skipped any jobs that are now guaranteed
				// to miss their deadline
				for (auto it = state_space_data.jobs_by_deadline.lower_bound(check_from);
					it != state_space_data.jobs_by_deadline.end(); it++) {
					const Job<Time>& j = *(it->second);
					auto pmin = j.get_min_parallelism();
					auto earliest = new_n.get_last_state()->core_availability(pmin).min();
					if (j.get_deadline() < earliest) {
						if (unfinished(new_n, j)) {
							DM("deadline miss: " << new_n << " -> " << j << std::endl);
							// This job is still incomplete but has no chance
							// of being scheduled before its deadline anymore.
							observed_deadline_miss = true;
							// if we stop at the first deadline miss, abort and create node in the graph for explanation purposes
							if (early_exit)
							{
								aborted = true;
								// create a dummy node for explanation purposes
								auto frange = new_n.get_last_state()->core_availability(pmin) + j.get_cost(pmin);
								Node& next =
									new_node(new_n, j, j.get_job_index(), state_space_data, 0, 0, 0);
								
								//const CoreAvailability empty_cav = {};
								State& next_s = new_state(*new_n.get_last_state(), j.get_job_index(), frange, frange, new_n.get_scheduled_jobs(), new_n.get_jobs_with_pending_successors(), new_n.get_ready_successor_jobs(), state_space_data, new_n.get_next_certain_source_job_release(), pmin);
								next.add_state(&next_s);
								num_states++;

								// update response times
								DM("update RESPONSE TIMES" << std::endl);
								update_finish_times(j, frange, (Time)-1, (Time)-1);
#ifdef CONFIG_COLLECT_SCHEDULE_GRAPH
								edges.emplace_back(&j, &new_n, &next, frange, pmin);
#endif
								count_edge();
							}
							break;
						}
					}
					else
						// deadlines now after the next earliest finish time
						break;
				}
			}

			bool all_jobs_scheduled(const Node& n)
			{
				return (n.number_of_scheduled_jobs() == state_space_data.num_jobs());
			}

			// find next time by which a job is certainly ready in system state 's'
			Time next_certain_job_ready_time(const Node& n, const State& s) const
			{
				Time t_ws = std::min(s.next_certain_gang_source_job_disptach(), s.next_certain_successor_jobs_disptach());
				Time t_wos = n.get_next_certain_sequential_source_job_release();
				return std::min(t_wos, t_ws);
			}

			// assumes j is ready
			// NOTE: we don't use Interval<Time> here because the
			//       Interval c'tor sorts its arguments.
			std::pair<Time, Time> start_times(
				const State& s, const Job<Time>& j, const Time t_wc, const Time t_high,
				const Time t_avail, const unsigned int ncores = 1) const
			{
				auto rt = state_space_data.earliest_ready_time(s, j);
				auto at = s.core_availability(ncores).min();
				Time est = std::max(rt, at);
				DM("This is job: " << j.get_job_index() << std::endl;)
				DM("rt: " << rt << std::endl
					<< "at: " << at << std::endl);

				Time lst = std::min(t_wc,
					std::min(t_high, t_avail) - Time_model::constants<Time>::epsilon());

				DM("est: " << est << std::endl);
				DM("lst: " << lst << std::endl);

				return { est, lst };
			}

			Time earliest_job_abortion(const Abort_action<Time>& a)
			{
				return a.earliest_trigger_time() + a.least_cleanup_cost();
			}

			Time latest_job_abortion(const Abort_action<Time>& a)
			{
				return a.latest_trigger_time() + a.maximum_cleanup_cost();
			}

			Interval<Time> calculate_abort_time(const Job<Time>& j, Time est, Time lst, Time eft, Time lft)
			{
				auto j_idx = j.get_job_index();
				auto abort_action = state_space_data.abort_action_of(j_idx);
				if (abort_action) {
					auto lt = abort_action->latest_trigger_time();
					// Rule: if we're certainly past the trigger, the job is
					//       completely skipped.
					if (est >= lt) {
						// job doesn't even start, it is skipped immediately
						return Interval<Time>{ est, lst };
					}
					else {
						// The job can start its execution but we check
						// if the job must be aborted before it finishes
						auto eat = earliest_job_abortion(*abort_action);
						auto lat = latest_job_abortion(*abort_action);
						return Interval<Time>{ std::min(eft, eat), std::min(lft, lat) };
					}
				}
				else {
					// compute range of possible finish times
					return Interval<Time>{ eft, lft };
				}
			}

			bool dispatch(const Node& n, const Job<Time>& j, Time t_wc_wos, Time t_high_wos)
			{
				// All states in node 'n' for which the job 'j' is eligible will 
				// be added to that same node. 
				// If such a node already exists, we keep a reference to it
				Node_ref next = nullptr;
				DM("--- global:dispatch() " << n << ", " << j << ", " << t_wc_wos << ", " << t_high_wos << std::endl);
				DM("Dispatched jobs in that node: " << n.get_scheduled_jobs() << std::endl);
				
				DM("Earliest pending release: " << n.earliest_job_release() << std::endl);
				
				//state_space_data.earliest_possible_job_release(n, j)

				bool dispatched_one = false;

				// loop over all states in the node n
				const auto* n_states = n.get_states();

#ifdef CONFIG_PARALLEL
				Nodes_map_accessor acc;
#endif	
	
				for (State* s : *n_states)
				{
			
					const auto& costs = j.get_all_costs();
					// check for all possible parallelism levels of the moldable gang job j (if j is not gang or not moldable than min_paralellism = max_parallelism and costs only constains a single element).
					//for (unsigned int p = j.get_max_parallelism(); p >= j.get_min_parallelism(); p--)
					for (auto it = costs.rbegin(); it != costs.rend(); it++)
					{
						unsigned int p = it->first;
						// Calculate t_wc and t_high
						Time t_wc = std::max(s->core_availability().max(), next_certain_job_ready_time(n, *s));

						Time t_high_succ = state_space_data.next_certain_higher_priority_successor_job_ready_time(n, *s, j, p, t_wc + 1);
						Time t_high_gang = state_space_data.next_certain_higher_priority_gang_source_job_ready_time(n, *s, j, p, t_wc + 1);
						Time t_high = std::min(t_high_wos, std::min(t_high_gang, t_high_succ));
						//DM("job:" << *state_space_data.get_earliest_arriving_job() << std::endl)
						// If j can execute on ncores+k cores, then 
						// the scheduler will start j on ncores only if 
						// there isn't ncores+k cores available
						Time t_avail = Time_model::constants<Time>::infinity();
						if (p < j.get_max_parallelism())
							t_avail = s->core_availability(std::prev(it)->first).max();

						DM("=== t_high = " << t_high << ", t_wc = " << t_wc << std::endl);
						auto _st = start_times(*s, j, t_wc, t_high, t_avail, p);
						if (_st.first > t_wc || _st.first >= t_high || _st.first >= t_avail)
							continue; // nope, not next job that can be dispatched in state s, try the next state.

						//calculate the job finish time interval
						auto exec_time = it->second;
						Time eft = _st.first + exec_time.min();
						Time lft = _st.second + exec_time.max();

						// check for possible abort actions
						Interval<Time> ftimes = calculate_abort_time(j, _st.first, _st.second, eft, lft);

						// yep, job j is a feasible successor in state s
						dispatched_one = true;

						//////////////////////////////////////////
						//Collecting all information here for now
						//////////////////////////////////////////
						Job_index j_idx = j.get_job_index();
						const Job_precedence_set &predecessors = state_space_data.predecessors_of(j_idx);
						Time max_pred_eft = 0;
						Time max_pred_lft = 0;
						for(auto pred: predecessors){
							std::cout << "IN dispatch: Job: " << j_idx << " pred: " << pred << std::endl;
							Job pred_j = state_space_data.jobs[pred];
							Time pred_min_cost = pred_j.least_exec_time();
							Time pred_max_cost = pred_j.maximal_exec_time();
							Time pred_est = s->saved_starts[pred].first;
							Time pred_lst = s->saved_starts[pred].second;
							Time pred_eft = pred_est + pred_min_cost;
							Time pred_lft = pred_lst + pred_max_cost;
							std::cout << "it's est: " << s->saved_starts[pred].first << " it's lst:" << s->saved_starts[pred].second << std::endl;
							if(pred_eft > max_pred_eft){max_pred_eft = pred_eft;}
							if(pred_lft > max_pred_lft){max_pred_lft = pred_lft;}
						}
						Time r_i_min = j.earliest_arrival();
						if(max_pred_eft > r_i_min){r_i_min = max_pred_eft;}
						Time r_i_max = j.latest_arrival();
						if(max_pred_lft > r_i_max){r_i_max = max_pred_lft;}
						Time r_bar_i_min = r_i_min;
						Time max_est_of_lpi = s->get_max_est_of_lpi(j_idx);
						if(max_est_of_lpi > r_bar_i_min){r_bar_i_min = max_est_of_lpi;}

						Time n_bcrt = exec_time.min();
						if((eft - r_i_max) > n_bcrt){n_bcrt = (eft - r_i_max);}
						t_avail = s->core_availability().max();
						Time n_wcrt = exec_time.max() + (t_avail - r_bar_i_min);
						std::cout << "##########################################################" << std::endl;
						std::cout << "exec_time.min(): " << exec_time.min() << std::endl;
						std::cout << "eft: " << eft << " r_i_max: " << r_i_max << " eft - r_i_max: " << eft - r_i_max << std::endl;
						std::cout << "r_bar_i_min: " << r_bar_i_min << std::endl;
						std::cout << "HERE n_bcrt: " << n_bcrt << " n_wcrt: " << n_wcrt << std::endl;
						std::cout << "exec_time.max(): " << exec_time.max() << " t_avail: " << t_avail << std::endl;
						std::cout << "##########################################################" << std::endl;
						//////////////////////////////////////////
						//Collecting all information here for now
						//////////////////////////////////////////

						// update finish-time estimates
						//This one is called normally, the other one is called if there is an early exit to finalize response times
						DM("update ESTIMATES" << std::endl);
						update_finish_times(j, ftimes, n_bcrt, n_wcrt);

					

#ifdef CONFIG_PARALLEL
						// if we do not have a pointer to a node with the same set of scheduled job yet,
						// try to find an existing node with the same set of scheduled jobs. Otherwise, create one.
						if (next == nullptr || acc.empty())
						{
							auto next_key = n.next_key(j);
							Job_set new_sched_jobs{ n.get_scheduled_jobs(), j.get_job_index() };

							while (next == nullptr || acc.empty()) {
								// check if key exists
								if (nodes_by_key.find(acc, next_key)) {
									// If be_naive, a new node and a new state should be created for each new job dispatch.
									if (be_naive) {
										next = &(new_node_at(acc, n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));
									}
									else
									{
										for (Node_ref other : acc->second) {
											if (other->get_scheduled_jobs() == new_sched_jobs) {
												next = other;
												DM("=== dispatch: next exists." << std::endl);
												break;
											}
										}
										if (next == nullptr) {
											next = &(new_node_at(acc, n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));
										}
									}
								}
								if (next == nullptr) {
									if (nodes_by_key.insert(acc, next_key)) {
										next = &(new_node_at(acc, n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));
									}
								}
								// if we raced with concurrent creation, try again
							}
						}
						// If be_naive, a new node and a new state should be created for each new job dispatch.
						else if (be_naive) {
							// note that the accessor should be pointing on something at this point
							next = &(new_node_at(acc, n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));
						}
						assert(!acc.empty());
#else
						// If be_naive, a new node and a new state should be created for each new job dispatch.
						if (be_naive)
							next = &(new_node(n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));

						// if we do not have a pointer to a node with the same set of scheduled job yet,
						// try to find an existing node with the same set of scheduled jobs. Otherwise, create one.
						if (next == nullptr)
						{
							const auto pair_it = nodes_by_key.find(n.next_key(j));
							if (pair_it != nodes_by_key.end()) {
								Job_set new_sched_jobs{ n.get_scheduled_jobs(), j.get_job_index() };
								for (Node_ref other : pair_it->second) {
									if (other->get_scheduled_jobs() == new_sched_jobs)
									{
										next = other;
										DM("=== dispatch: next exists." << std::endl);
										break;
									}
								}
							}
							// If there is no node yet, create one.
							if (next == nullptr){
								DM("Created here 1?" << std::endl);
								next = &(new_node(n, j, j.get_job_index(), state_space_data, state_space_data.earliest_possible_job_release(n, j), state_space_data.earliest_certain_source_job_release(n, j), state_space_data.earliest_certain_sequential_source_job_release(n, j)));
							}
						}
#endif
						// next should always exist at this point, possibly without states in it
						// create a new state resulting from scheduling j in state s on p cores and try to merge it with an existing state in node 'next'.							
						new_or_merge_state(*next, *s, j.get_job_index(),
							Interval<Time>{_st}, ftimes, next->get_scheduled_jobs(), next->get_jobs_with_pending_successors(), next->get_ready_successor_jobs(), state_space_data, next->get_next_certain_source_job_release(), p);

						// make sure we didn't skip any jobs which would then certainly miss its deadline
						// only do that if we stop the analysis when a deadline miss is found 
						if (be_naive && early_exit) {
							check_for_deadline_misses(n, *next);
						}

#ifdef CONFIG_COLLECT_SCHEDULE_GRAPH
						edges.emplace_back(&j, &n, next, ftimes);
#endif
						count_edge();
					}
				}

				// if we stop the analysis when a deadline miss is found, then check whether a job will certainly miss 
				// its deadline because of when the processors become free next.
				// if we are not using the naive exploration, we check for deadline misses only once per job dispatched
				if (early_exit && !be_naive && next != nullptr)
					check_for_deadline_misses(n, *next);

				return dispatched_one;
			}

			void explore(const Node& n)
			{
				bool found_one = false;

				DM("---- global:explore(node)" << n.finish_range() << std::endl);

				// (0) define the window of interest
				auto t_min = n.earliest_job_release();
				// latest time some unfinished job is certainly ready
				auto nxt_ready_job = n.next_certain_job_ready_time();
				// latest time all cores are certainly available
				auto avail_max = n.latest_core_availability();
				// latest time by which a work-conserving scheduler
				// certainly schedules some job
				auto upbnd_t_wc = std::max(avail_max, nxt_ready_job);

				DM("This is n: " << n << std::endl);
				DM("t_min: " << t_min << std::endl
					<< "nxt_ready_job: " << nxt_ready_job << std::endl
					<< "avail_max: " << avail_max << std::endl
					<< "upbnd_t_wc: " << upbnd_t_wc << std::endl);

				//check all jobs that may be eligible to be dispatched next
				// part 1: check source jobs (i.e., jobs without prcedence constraints) that are potentially eligible
				for (auto it = state_space_data.jobs_by_earliest_arrival.lower_bound(t_min);
					it != state_space_data.jobs_by_earliest_arrival.end();
					it++)
				{
					const Job<Time>& j = *it->second;
					DM(j << " (" << j.get_job_index() << ")" << std::endl);
					DM(j << " unfinished?: " << unfinished(n,j) << std::endl);
					// stop looking once we've left the window of interest
					if (j.earliest_arrival() > upbnd_t_wc)
						break;

					if (!unfinished(n, j))
						continue;

					Time t_high_wos = state_space_data.next_certain_higher_priority_seq_source_job_release(n, j, upbnd_t_wc + 1);
					// if there is a higher priority job that is certainly ready before job j is released at the earliest, 
					// then j will never be the next job dispached by the scheduler
					if (t_high_wos <= j.earliest_arrival())
						continue;
					found_one |= dispatch(n, j, upbnd_t_wc, t_high_wos);
				}
				// part 2: check ready successor jobs (i.e., jobs with precedence constraints that are completed) that are potentially eligible
				for (auto it = n.get_ready_successor_jobs().begin();
					it != n.get_ready_successor_jobs().end();
					it++)
				{
					const Job<Time>& j = **it;
					DM(j << " (" << j.get_job_index() << ")" << std::endl);
					// stop looking once we've left the window of interest
					if (j.earliest_arrival() > upbnd_t_wc)
						continue;

					// Since this job is is recorded as ready in the state, it better
					// be incomplete...
					assert(unfinished(n, j));

					Time t_high_wos = state_space_data.next_certain_higher_priority_seq_source_job_release(n, j, upbnd_t_wc + 1);
					// if there is a higher priority job that is certainly ready before job j is released at the earliest, 
					// then j will never be the next job dispached by the scheduler
					if (t_high_wos <= j.earliest_arrival())
						continue;
					found_one |= dispatch(n, j, upbnd_t_wc, t_high_wos);
				}

				// check for a dead end
				if (!found_one && !all_jobs_scheduled(n)) {
					// out of options and we didn't schedule all jobs
					observed_deadline_miss = true;
					aborted = true;
				}
			}

			// naive: no state merging
			void explore_naively()
			{
				be_naive = true;
				explore();
			}

			void explore()
			{
				int last_time;
				unsigned int target_depth;
				
				if (verbose) {
					std::cout << "0%";
					last_time = get_cpu_time();
					target_depth = std::max((unsigned int)state_space_data.num_jobs(), max_depth);
				}

				int last_num_states = 0;
				make_initial_node(num_cpus);

				while (current_job_count < state_space_data.num_jobs()) {
					unsigned long n;
#ifdef CONFIG_PARALLEL
					const auto& new_nodes_part = nodes_storage.back();
					n = 0;
					for (const Nodes& new_nodes : new_nodes_part) {
						n += new_nodes.size();
					}
#else
					Nodes& exploration_front = nodes();
					n = exploration_front.size();
#endif
					if (n == 0)
					{
						aborted = true;
						break;
					}
					// allocate node space for next depth
					nodes_storage.emplace_back();

					// keep track of exploration front width
					max_width = std::max(max_width, n);
					width[current_job_count] = { n, num_states - last_num_states };
					last_num_states = num_states;

					if (verbose) {
						int time = get_cpu_time(); 
						if (time > last_time+4) { // update progress information approxmately every 4 seconds of runtime
							std::cout << "\r" << (int)(((double)current_job_count / target_depth) * 100) << "%";
							last_time = time;
						}
					}

					check_depth_abort();
					check_cpu_timeout();
					if (aborted)
						break;

#ifdef CONFIG_PARALLEL

					parallel_for(new_nodes_part.range(),
						[&](typename Split_nodes::const_range_type& r) {
							for (auto it = r.begin(); it != r.end(); it++) {
								const Nodes& new_nodes = *it;
								auto s = new_nodes.size();
								tbb::parallel_for(tbb::blocked_range<size_t>(0, s),
									[&](const tbb::blocked_range<size_t>& r) {
										for (size_t i = r.begin(); i != r.end(); i++)
											explore(new_nodes[i]);
									});
							}
						});

#else
					for (const Node& n : exploration_front) {
						explore(n);
						check_cpu_timeout();
						if (aborted)
							break;
					}
#endif

					// clean up the state cache if necessary
					if (!be_naive)
						nodes_by_key.clear();

					current_job_count++;

#ifndef CONFIG_COLLECT_SCHEDULE_GRAPH
					// If we don't need to collect all nodes, we can remove
					// all those that we are done with, which saves a lot of
					// memory.
#ifdef CONFIG_PARALLEL
					parallel_for(nodes_storage.front().range(),
						[](typename Split_nodes::range_type& r) {
							for (auto it = r.begin(); it != r.end(); it++)
								it->clear();
						});
#endif
					nodes_storage.pop_front();
#endif

				}
				if (verbose)
					std::cout << "\r100%" << std::endl << "Terminating" << std::endl;

#ifdef CONFIG_PARALLEL
				// propagate any updates to the response-time estimates
				for (auto& r : partial_rta) {
					for (int i = 0; i < r.size(); ++i) {
						if (r[i].valid)
							update_finish_times(rta, i, r[i].rt);
					}
				}
#endif


#ifndef CONFIG_COLLECT_SCHEDULE_GRAPH
				// clean out any remaining nodes
				while (!nodes_storage.empty()) {
#ifdef CONFIG_PARALLEL
					parallel_for(nodes_storage.front().range(),
						[](typename Split_nodes::range_type& r) {
							for (auto it = r.begin(); it != r.end(); it++)
								it->clear();
						});
#endif
					nodes_storage.pop_front();
				}
#endif

#ifdef CONFIG_PARALLEL
				for (auto& c : edge_counter)
					num_edges += c;
#endif
			}


#ifdef CONFIG_COLLECT_SCHEDULE_GRAPH
			friend std::ostream& operator<< (std::ostream& out,
				const State_space<Time>& space)
			{
				std::map<const Schedule_node<Time>*, unsigned int> node_id;
				unsigned int i = 0;
				out << "digraph {" << std::endl;
#ifdef CONFIG_PARALLEL
				for (const Split_nodes& nodes : space.get_nodes()) {
					for (const Schedule_node<Time>& n : tbb::flattened2d<Split_nodes>(nodes)) {
#else
				for (const auto& front : space.get_nodes()) {
					for (const Schedule_node<Time>& n : front) {
#endif
						/*node_id[&n] = i++;
						out << "\tS" << node_id[&n]
							<< "[label=\"S" << node_id[&n] << ": ";
						n.print_vertex_label(out, space.jobs);
						out << "\"];" << std::endl;*/
						node_id[&n] = i++;
						out << "\tN" << node_id[&n]
							<< "[label=\"N" << node_id[&n] << ": {";
						const auto* n_states = n.get_states();

						for (State* s : *n_states)
						{
							out << "[";
							s->print_vertex_label(out, space.state_space_data.jobs);
							//<< s->earliest_finish_time()
							//<< ", "
							//<< s->latest_finish_time()
							out << "]\\n";
						}
						out << "}"
							<< "\\nER=";
						if (n.earliest_job_release() ==
							Time_model::constants<Time>::infinity()) {
							out << "N/A";
						}
						else {
							out << n.earliest_job_release();
						}
						out << "\"];"
							<< std::endl;
					}
				}
				for (const auto& e : space.get_edges()) {
					out << "\tN" << node_id[e.source]
						<< " -> "
						<< "N" << node_id[e.target]
						<< "[label=\""
						<< "T" << e.scheduled->get_task_id()
						<< " J" << e.scheduled->get_job_id()
						<< "\\nDL=" << e.scheduled->get_deadline()
						<< "\\nES=" << e.earliest_start_time()
						<< "\\nLS=" << e.latest_start_time()
						<< "\\nEF=" << e.earliest_finish_time()
						<< "\\nLF=" << e.latest_finish_time()
						<< "\"";
					if (e.deadline_miss_possible()) {
						out << ",color=Red,fontcolor=Red";
					}
					out << ",fontsize=8" << "]"
						<< ";"
						<< std::endl;
					if (e.deadline_miss_possible()) {
						out << "N" << node_id[e.target]
							<< "[color=Red];"
							<< std::endl;
					}
				}
				out << "}" << std::endl;
				return out;
					}
#endif
				};

			}
		}

namespace std
{
	template<class Time> struct hash<NP::Global::Schedule_state<Time>>
	{
		std::size_t operator()(NP::Global::Schedule_state<Time> const& s) const
		{
			return s.get_key();
		}
	};
}


#endif
