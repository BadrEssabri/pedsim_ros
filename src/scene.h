#ifndef SCENE_TEST_H
#define SCENE_TEST_H

#include <ped_scene.h>
#include <ped_tree.h>

#include <QMap>
#include <QTimer>
#include <QKeyEvent>
#include <QGraphicsScene>

#include <boost/bind.hpp>

#include <qwt/qwt_color_map.h>
#include <qwt/qwt_interval.h>

#include <logging.h>
#include <config.h>
#include <grid.h>
#include <agent.h>
#include <obstacle.h>
#include <waypoint.h>

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>


// ros and big guys
#include <ros/ros.h>
#include <ros/console.h>
#include <boost/shared_ptr.hpp>

// messages and services
#include <pedsim_msgs/AgentState.h>
#include <pedsim_msgs/AllAgentsState.h>

#include <pedsim_srvs/SetAgentState.h>
#include <pedsim_srvs/SetAllAgentsState.h>
#include <pedsim_srvs/GetAgentState.h>
#include <pedsim_srvs/GetAllAgentsState.h>


class QGraphicsScene;
class Agent;
class Grid;
class Obstacle;
class Waypoint;
class GNode;



class Scene : public QObject, public Ped::Tscene
{
    Q_OBJECT
public:
    Scene(QGraphicsScene* guiSceneIn, const ros::NodeHandle& node);
    virtual ~Scene();

    bool isPaused() const;
    void pauseUpdates();
    void unpauseUpdates();
    void clear();
    static Grid* getGrid();
    void initializeAll();
    std::set<const Ped::Tagent*> getNeighbors(double x, double y, double maxDist);

    /// service handler for moving agents
    bool srvMoveAgentHandler(pedsim_srvs::SetAgentState::Request&, pedsim_srvs::SetAgentState::Response& );
    void publicAgentStatus();

public slots:
    void updateWaypointVisibility(bool visible);

protected slots:
    void moveAllAgents();
    void cleanupSlot();

public:
    QGraphicsScene* guiScene;
    QList<Agent*> agents;
    QList<Obstacle*> obstacles;
    QMap<QString, Waypoint*> waypoints;
    static Grid* grid_;
    size_t timestep;

private:
    QTimer movetimer;
    QTimer cleanuptimer;

    // robot and agents
    Ped::Tagent* robot_;
    std::vector<Ped::Tagent*> all_agents_;

    ros::NodeHandle nh_;

    // publishers
    ros::Publisher pub_all_agents_;
    ros::ServiceServer srv_move_agent_;

    double eDist(double x1, double y1, double x2, double y2)
    {
        double dx = (x2-x1) * (1/20.0);
        double dy = (y2-y1) * (1/20.0);

        return sqrt( pow(dx, 2.0) + pow(dy, 2.0) );    // distance in metres
    }
};



#endif // SCENE_ICRA14_H
