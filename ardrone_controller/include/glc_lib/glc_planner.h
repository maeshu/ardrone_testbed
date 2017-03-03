#ifndef GLC_PLANNER_H
#define GLC_PLANNER_H

#include <map>
#include <limits>
#include <memory>
#include <stack>
#include <queue>
#include <deque>
#include "user_interface.h"
#include "glc_utils.h"

namespace glcm{  
    
    class trajectory_planner
    {
    public:
        int num;
        //best path to goal
        glcm::nodePtr best;
        //initial condition
        glcm::nodePtr root_ptr;
        //pointer to the ode integrator object
        glcm::dynamical_system* dynamics;
        // goal region
        glcm::goalRegion* goal;
        // obstacle
        glcm::obstacles* obs;
        // cost function
        glcm::costFunction* cf;
        // heuristic 
        heuristic* h;
        //queue of nodes
        std::priority_queue<glcm::nodePtr,
        std::vector<glcm::nodePtr>,
        glcm::queue_order> queue;
        //create the set with type and compare function
        std::set<glcm::domain> domain_labels;
        //iterator type for sets
        std::set<glcm::domain>::iterator it;
        //upper bound on known optimal cost
        double UPPER;
        //tolerance on cost difference between two related controls
        double eps;
        //maximum search depth
        int depth_limit;
        //variable for the simulation time of the expand function
        double expand_time;
        //initial scaling of partition size
        double partition_scale;
        //Timer
        clock_t t, tstart;
        //iteration count
        int32_t iter;
        //If a solution is found
        bool foundGoal,live;
        //parameters
        glcm::parameters params;
        //control grid
        std::deque<vctr> controls;
        //children expanded
        int sim_count;
        //eta function
        double eta;
        
        //PLanner tree handling functions
        void add_child(nodePtr parent, nodePtr child);
        void switch_root(); // temp
        void remove_subtree(nodePtr& root); // TODO domains should clear if empty
        std::vector<nodePtr> path_to_root(bool forward=false);
        traj recover_traj(const std::vector<nodePtr>& path);
        
        
        //Planner methods
        void expand();
        void plan(plannerOutput& out);
        void plan();
        bool get_solution(traj& traj_out);
        
        
        //ctor
        trajectory_planner(obstacles* _obs, 
                           goalRegion* _goal, 
                           dynamical_system* _dynamics, 
                           heuristic* _h,
                           costFunction* _cf,
                           const parameters& _params,
                           const std::deque<vctr>& _controls
        ):
        params(_params), 
        dynamics(_dynamics),
        obs(_obs), 
        goal(_goal),
        cf(_cf),
        h(_h)
        {
            controls=_controls;
            best = glcm::node::inf_cost_node;
            UPPER=DBL_MAX/2.0;
            foundGoal=false;
            live=true;
            root_ptr = glcm::nodePtr(new glcm::node(params,controls.size()));
            root_ptr->merit=h->cost_to_go(root_ptr->x);
            glcm::domain d0(root_ptr);
            //Add root to search containers
            queue.push(root_ptr);
            domain_labels.insert(d0);
            ////////////*Scaling functions*//////////////
            // 1/R
            expand_time=params.time_scale/(double)params.res;
            //h(R)
            depth_limit=params.depth_scale*params.res*floor(log(params.res));
            //eta(R) \in \little-omega (log(R)*R^L_f)
            eta = log(params.res)*log(params.res)*pow(params.res,dynamics->Lip_flow)/( params.partition_scale );
            partition_scale=eta/( params.partition_scale );
            //eps(R)
            if(cf->Lip_cost>0)
            {
                 eps = (sqrt(params.state_dim)/partition_scale)*
                (dynamics->Lip_flow)/(cf->Lip_cost)*
                (params.res*exp(dynamics->Lip_flow)-1.0);
            }
            else
            {
                eps=0;
            }
            
            /////////*Monitor Performance*/////////////
            std::cout << "\n\n\n\nPre-search summary:\n" << std::endl;
            std::cout << "        Threshold: " << eps << std::endl;
            std::cout << "      Expand time: " << expand_time << std::endl;
            std::cout << "      Depth limit: " << depth_limit <<  std::endl;
            std::cout << "      Domain size: " << 1.0/eta << std::endl;
            std::cout << "   Max iterations: " << params.max_iter << std::endl;
            
            iter=0;
            sim_count=0;
            t = clock();
            tstart = clock();
        }
        
        
    };
    
    void trajectory_planner::add_child(nodePtr parent, nodePtr child){
        child->parent = parent;
        child->depth = parent->depth+1;
        child->t = (parent->t+expand_time);
        parent->children[child->u_idx] = child;
    }
    
    void trajectory_planner::expand()
    {
        
        iter++;
        if(queue.size()==0){
            std::cout << "---The queue is empty. Finished planning---" << std::endl;
            live=false;
            return;
        }
        
        glcm::nodePtr current_node = queue.top();
        queue.pop();
        
        if(current_node->depth >=depth_limit)
        {
            std::cout << "exceeded depth limit " << std::endl;
            live=false;
            return;
        }
        
        //A set of domains visited by new nodes made by expand
        std::set<glcm::domain*> domains_needing_update; 
        std::map<glcm::nodePtr, glcm::traj> traj_from_parent;
        
        //Expand top of queue and store arcs in 
        for(int i=0;i<controls.size();i++)
        {
            glcm::nodePtr new_arc(new glcm::node(controls.size()));
            traj_from_parent[new_arc] = dynamics->sim( current_node->t, current_node->t+expand_time , current_node->x, controls[i]);//not the intended use of path member
            new_arc->cost = cf->cost(traj_from_parent[new_arc], controls[i])+current_node->cost;
            new_arc->merit = new_arc->cost + h->cost_to_go(traj_from_parent[new_arc].states.back());
            new_arc->u_idx = i;
            new_arc->x = traj_from_parent[new_arc].states.back();
            new_arc->t = traj_from_parent[new_arc].time.back();
            
            vctr w = partition_scale*(traj_from_parent[new_arc].states.back());
            glcm::domain d_new;
            d_new.coordinate = vec_floor( w );
            
            
            // the following yields a reference to either the new domain or the existing one
            glcm::domain& bucket = const_cast<glcm::domain&>( *(domain_labels.insert(d_new).first) );
            domains_needing_update.insert(&bucket);
            
            //Add new_arc to the candidates queue for collision checking since it cant be discarded
            if(new_arc->cost < bucket.label->cost+eps /* and mew_arc->t <= bucket.label->t*/)
            {
                bucket.candidates.push(new_arc);//These arcs can potentially get pushed to main Q and even relabel region.
            }     
        }
        
        for(auto& open_domain: domains_needing_update)
        {
            
            glcm::domain& current_domain = *open_domain; // HACK[?]
            
            //We go through the queue of candidates for relabeling/pushing in each set
            bool found_best = false;
            while( not current_domain.candidates.empty())
            {
                //Check that the top of the domain's candidate queue is within the current threshold
                if(current_domain.candidates.top()->cost < current_domain.label->cost+eps){
                    //                 if(current_domain.candidates.top()->merit < current_domain.label->merit+eps){
                    
                    const glcm::nodePtr& curr = current_domain.candidates.top(); 
                    //Anything collision free within the threshold has to stay. Push to queue. 
                    //The first one that's coll free becomes new label 
                    if(obs->collisionFree(traj_from_parent[curr]))
                    {
                        add_child(current_node, curr);
                        if(not foundGoal)
                        {
                            queue.push(curr);//anything coll free at this point goes to queue
                        }
                        //the cheapest one relabels the domain
                        if(!found_best){
                            found_best = true;
                            current_domain.label = curr;
                        }
                        
                        //Goal checking
                        if(goal->inGoal(traj_from_parent[curr],&num) and curr->cost < best->cost)
                        {
                            t = clock() - tstart;
                            foundGoal=true;
                            live=false;
                            std::cout << "\n\nFound goal at iter: " << iter << std::endl;
                            best=curr;
                            //TODO not consistent with anything other than min-time cost here
                            double tail_cost = (traj_from_parent[curr].time.back()-traj_from_parent[curr].time[num-1])*
                            (1.0+(cf->Lip_cost)*sqr(controls[best->u_idx][0]));
                            std::cout << "         Tail cost: " << tail_cost << std::endl;
                            std::cout << "    cost from root: " << curr->cost << std::endl;
                            std::cout << "          End time: " <<  traj_from_parent[curr].time.back() << std::endl;
                            UPPER=curr->cost-tail_cost;
                            
                        }    
                    }
                    
                }
                current_domain.candidates.pop();//TODO if current_domain.label = NULL (or whatever the default is) delete the domain.
            }
            
            if(current_domain.empty()){
                // Prevent domains without a path 
                domain_labels.erase(current_domain);
            }
        }
        
        
        return;
    }
    
    void trajectory_planner::plan(plannerOutput& out)
    {
        while(live)
        {
            expand();
        }
        out.cost=UPPER;
        out.time=(float) t/ (float) CLOCKS_PER_SEC;
        return;
    }
    
    void trajectory_planner::plan()
    {
        while(live)
        {
            expand();
        }
        
        return;
    }
    
    //get the nodePtr path to the root with the order specified by foward
    std::vector<nodePtr> trajectory_planner::path_to_root(bool forward)
    {
        nodePtr currentNode = best;
        std::vector<nodePtr> path;
        while( not (currentNode->parent == nullptr) )
        {
            path.push_back(currentNode);
            currentNode=currentNode->parent;
        }
        path.push_back(currentNode);
        
        if(forward)
            std::reverse(path.begin(),path.end());
        return path;
    }
    
    //return the planned trajectory
    traj trajectory_planner::recover_traj(const std::vector<nodePtr>& path)
    {
        int i=0;
        double t0=0;
        double tf=expand_time;
        glcm::nodePtr current = path[0];
        glcm::traj arc,opt_sol;
        if(path.size()<2)
        {
            opt_sol.clear();
            return opt_sol;
        }
        
        
        //recalculate arcs connecting nodes
        for(int i=0; i<path.size()-1;i++)
        {
            dynamics->sim(arc, t0, tf, path[i]->x,controls[path[i+1]->u_idx]);
            if(i<path.size()-2)
            {
                arc.pop_back();
            }
            opt_sol.push(arc);
            t0 = arc.time.back();
            tf = t0+expand_time;
        }
        
        return opt_sol;
    }
}//close namespace
#endif