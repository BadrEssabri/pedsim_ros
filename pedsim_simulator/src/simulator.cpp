/**
 * Copyright 2014-2016 Social Robotics Lab, University of Freiburg
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    # Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    # Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    # Neither the name of the University of Freiburg nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \author Billy Okal <okal@cs.uni-freiburg.de>
 * \author Sven Wehner <mail@svenwehner.de>
 */


#include "pedsim_simulator/element/agentcluster.hpp"
#include "pedsim_simulator/scene.hpp"
#include "pedsim_simulator/simulator.hpp"

#include "pedsim_utils/geometry.hpp"

using namespace pedsim;
using namespace pedsim_msgs::msg;
using std::placeholders::_1;
using std::placeholders::_2;


Simulator::Simulator() : Node("pedsim_simulator") {

  this->initializeParams();
  this->initializeSimulation();
  this->runSimulation();
}

void Simulator::initializeParams() {
  // load additional parameters
  this->declare_parameter("odom_frame_id", "odom");
  this->declare_parameter("base_frame_id", "base_footprint");
  this->declare_parameter("groups_enabled", true);
  this->declare_parameter("max_robot_speed", 1.5);
  this->declare_parameter("update_rate", 25.0);
  this->declare_parameter("simulation_factor", 1.0);
  this->declare_parameter("op_mode", 1);
  this->declare_parameter("queue_size", 10);
  this->declare_parameter("robot_radius", 0.35);
  this->declare_parameter("agent_radius", 0.35);
  this->declare_parameter("force_social", 10.0);
  // Soartoros, arrived human
  this->declare_parameter("human_arrived", "false");


  odom_frame_id =this->get_parameter("odom_frame_id").as_string();
  base_frame_id =this->get_parameter("base_frame_id").as_string();
  groups_enabled = this->get_parameter("groups_enabled").as_bool();
  max_robot_speed = this->get_parameter("max_robot_speed").as_double();
  update_rate = this->get_parameter("update_rate").as_double();
  simulation_factor = this->get_parameter("simulation_factor").as_double();
  op_mode = this->get_parameter("op_mode").as_int();
  robot_mode = static_cast<RobotMode>(op_mode);
  queue_size = this->get_parameter("queue_size").as_int();
  robot_radius = this->get_parameter("robot_radius").as_double();
  agent_radius = this->get_parameter("agent_radius").as_double();
  forceSocial = this->get_parameter("force_social").as_double();
  arrived_human = this->get_parameter("human_arrived").as_string();

}


bool Simulator::initializeSimulation() {
  std::string scene_file_param;
  paused_ = false;
  robot_ = nullptr;


  this->declare_parameter("scene_file", "");
  scene_file_param = this->get_parameter("scene_file").as_string();
  if (scene_file_param == "") {
    RCLCPP_ERROR_STREAM(get_logger(),
                        "Invalid scene file: " << scene_file_param);
    return false;
  }

  RCLCPP_INFO_STREAM(get_logger(), "Loading scene [" << scene_file_param
                                                     << "] for simulation");

  const QString scenefile = QString::fromStdString(scene_file_param);
  ScenarioReader scenario_reader;
  if (scenario_reader.readFromFile(scenefile) == false) {
    RCLCPP_ERROR_STREAM(
        get_logger(),
        "Could not load the scene file, please check the paths and param "
        "names : "
            << scene_file_param);
    return false;
  }

  RCLCPP_INFO_STREAM(
      get_logger(),
      "Using default queue size of "
          << queue_size << " for publisher queues... "
          << (queue_size == 0
                  ? "NOTE: This means the queues are of infinite size!"
                  : ""));

  // setup ros2 publishers
  pub_obstacles_ = create_publisher<LineObstacles>(
      "pedsim_simulator/simulated_walls", queue_size);
  pub_agent_states_ = create_publisher<AgentStates>(
      "pedsim_simulator/simulated_agents", queue_size);
  pub_agent_groups_ = create_publisher<AgentGroups>(
      "pedsim_simulator/simulated_groups", queue_size);
  pub_robot_position_ = create_publisher<nav_msgs::msg::Odometry>(
      "pedsim_simulator/robot_position", queue_size);

  // ros2 pub for hunav_eval
  pub_human_states_ = create_publisher<hunav_msgs::msg::Agents>(
      "/human_states", queue_size);
  pub_robot_state_ = create_publisher<hunav_msgs::msg::Agent>(
      "/robot_states", queue_size);

  // services
  srv_pause_simulation_ = create_service<std_srvs::srv::Empty>(
      "pedsim_simulator/pause_simulation",
      std::bind(&Simulator::onPauseSimulation, this, _1, _2));
  srv_unpause_simulation_ = create_service<std_srvs::srv::Empty>(
      "pedsim_simulator/unpause_simulation",
      std::bind(&Simulator::onUnpauseSimulation, this, _1, _2));
  // setup TF2 listener
  tf_buffer_ = std::make_shared<tf2_ros::Buffer>(get_clock());
  auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
      get_node_base_interface(), get_node_timers_interface());
  tf_buffer_->setCreateTimerInterface(timer_interface);
  tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

  spawn_timer_ =
      create_wall_timer(1000ms, std::bind(&Simulator::spawnCallback, this));
  return true;
}

void Simulator::runSimulation() {
  rclcpp::Rate r(update_rate);
  while (rclcpp::ok()) {
    if (!robot_) {
      // setup the robot
      for (Agent *agent : SCENE.getAgents()) {
        agent->setForceFactorSocial(forceSocial);
        if (agent->getType() == Ped::Tagent::ROBOT) {
          robot_ = agent;
          agent->SetRadius(robot_radius);
          last_robot_orientation_ =
              poseFrom2DVelocity(robot_->getvx(), robot_->getvy());
        } else
          agent->SetRadius(agent_radius);
      }
    }
    if (!paused_) {
      updateRobotPositionFromTF();
      SCENE.moveAllAgents();
      publishAgents();
      publishGroups();
      publishRobotPosition();
      publishObstacles(); // TODO - no need to do this all the time.
    }

    r.sleep();
  }
}

void Simulator::onPauseSimulation(
    const std::shared_ptr<std_srvs::srv::Empty::Request> request,
    std::shared_ptr<std_srvs::srv::Empty::Response> response) {
  paused_ = true;
}

void Simulator::onUnpauseSimulation(
    const std::shared_ptr<std_srvs::srv::Empty::Request> request,
    std::shared_ptr<std_srvs::srv::Empty::Response> response) {
  paused_ = false;
}

void Simulator::spawnCallback() {
  RCLCPP_DEBUG(get_logger(), "Spawning new agents.");

  for (const auto &sa : SCENE.getSpawnAreas()) {
    AgentCluster *agentCluster = new AgentCluster(sa->x, sa->y, sa->n);
    agentCluster->setDistribution(sa->dx, sa->dy);
    agentCluster->setType(static_cast<Ped::Tagent::AgentType>(0));

    for (const auto &wp_name : sa->waypoints) {
      agentCluster->addWaypoint(SCENE.getWaypointByName(wp_name));
    }

    SCENE.addAgentCluster(agentCluster);
  }
}

void Simulator::updateRobotPositionFromTF() {
  if (!robot_)
    return;

  if (robot_mode == RobotMode::TELEOPERATION ||
      robot_mode == RobotMode::CONTROLLED) {
    robot_->setTeleop(true);
    robot_->setVmax(max_robot_speed);
    geometry_msgs::msg::TransformStamped tf_msg;
    try {
      // Check if the transform is available
      tf_msg = tf_buffer_->lookupTransform(odom_frame_id, base_frame_id,
                                           tf2::TimePointZero);
    } catch (tf2::TransformException &e) {
      auto &clk = *this->get_clock();
      RCLCPP_WARN_THROTTLE(get_logger(), clk, 5000, "%s", e.what());
      return;
    }
    tf2::Transform tfTransform;
    tf2::impl::Converter<true, false>::convert(tf_msg.transform, tfTransform);

    const double x = tfTransform.getOrigin().x();
    const double y = tfTransform.getOrigin().y();
    const double dx = x - last_robot_pose_.transform.translation.x,
                 dy = y - last_robot_pose_.transform.translation.y;
    // const double dt = tf_msg.header.stamp.sec - last_robot_pose_.header.stamp.sec;
    const double dt = (tf_msg.header.stamp.sec + (tf_msg.header.stamp.nanosec * 1e-9)) 
                - (last_robot_pose_.header.stamp.sec + (last_robot_pose_.header.stamp.nanosec * 1e-9));

    double vx = dx / dt, vy = dy / dt;

    if (!std::isfinite(vx))
      vx = 0;
    if (!std::isfinite(vy))
      vy = 0;

    robot_->setX(x);
    robot_->setY(y);
    robot_->setvx(vx);
    robot_->setvy(vy);

    last_robot_pose_ = tf_msg;
  }
}

void Simulator::publishRobotPosition() {
  if (robot_ == nullptr)
    return;

  nav_msgs::msg::Odometry robot_location;
  robot_location.header = createMsgHeader();
  robot_location.child_frame_id = base_frame_id;

  robot_location.pose.pose.position.x = robot_->getx();
  robot_location.pose.pose.position.y = robot_->gety();
  if (hypot(robot_->getvx(), robot_->getvy()) < 0.05) {
    robot_location.pose.pose.orientation = last_robot_orientation_;
  } else {
    robot_location.pose.pose.orientation =
        poseFrom2DVelocity(robot_->getvx(), robot_->getvy());
    last_robot_orientation_ = robot_location.pose.pose.orientation;
  }

  robot_location.twist.twist.linear.x = robot_->getvx();
  robot_location.twist.twist.linear.y = robot_->getvy();

  pub_robot_position_->publish(robot_location);
}

void Simulator::publishAgents() {
  if (SCENE.getAgents().size() < 2) {
    return;
  }

  // Check if followed human has arrived
  arrived_human = this->get_parameter("human_arrived").as_string();

  AgentStates all_status;
  all_status.header = createMsgHeader();

  auto VecToMsg = [](const Ped::Tvector &v) {
    geometry_msgs::msg::Vector3 gv;
    gv.x = v.x;
    gv.y = v.y;
    gv.z = v.z;
    return gv;
  };

  // Define agent data for hunavsim evaluator
  hunav_msgs::msg::Agents agents_eval;
  hunav_msgs::msg::Agent robot_eval;
  agents_eval.header = createMsgHeader();

  for (const Agent *a : SCENE.getAgents()) {
    hunav_msgs::msg::Agent agent_eval;
    hunav_msgs::msg::AgentBehavior agent_behav_eval;

    agent_eval.id = a->getId();
    agent_eval.type = 1;
    agent_eval.skin = 0;
    agent_eval.name = "agent" + std::to_string(a->getId());
    agent_eval.group_id = -1;
    agent_eval.position.position.x = a->getx();
    agent_eval.position.position.y = a->gety();
    agent_eval.position.position.z = a->getz();
    auto theta = std::atan2(a->getvy(), a->getvx());
    agent_eval.position.orientation = pedsim::angleToQuaternion(theta);
    agent_eval.yaw = theta;
    agent_eval.velocity.linear.x = a->getvx();
    agent_eval.velocity.linear.y = a->getvy();
    agent_eval.velocity.linear.z = a->getvz();
    agent_eval.desired_velocity = 0.3;
    agent_eval.radius = 0.2;
    agent_eval.linear_vel = std::pow((std::pow(a->getvx(), 2) + std::pow(a->getvy(), 2)), 0.5);
    agent_eval.angular_vel = 0.0;

    agent_behav_eval.type = 1;
    agent_behav_eval.state = 1;
    agent_behav_eval.configuration = 0;
    agent_behav_eval.duration = 40.0;
    agent_behav_eval.once = true;
    agent_behav_eval.vel = 0.0;
    agent_behav_eval.dist = 0.0;
    agent_behav_eval.social_force_factor = 0.0;
    agent_behav_eval.goal_force_factor = 0.0;
    agent_behav_eval.obstacle_force_factor = 0.0;
    agent_behav_eval.other_force_factor = 0.0;

    agent_eval.behavior = agent_behav_eval;

    // geometry_msgs/Pose[] goals, uninit
    agent_eval.cyclic_goals = true;
    agent_eval.goal_radius = 0.2;

    Ped::Tvector v = a->getObstacleForce();

    geometry_msgs::msg::Point obstacleforce;
    obstacleforce.x = v.x;
    obstacleforce.y = v.y;
    obstacleforce.z = v.z;
    agent_eval.closest_obs.push_back(obstacleforce);

    // Could reorder function that works with this in metrics, by directly publishing the obstacle force
    // TODO: make sure the start of the recording is synced to work with this method

    // pedsim agent state
    AgentState state;
    state.header = createMsgHeader();

    state.id = a->getId();
    state.type = a->getType();
    state.pose.position.x = a->getx();
    state.pose.position.y = a->gety();
    state.pose.position.z = a->getz();
    state.pose.orientation = pedsim::angleToQuaternion(theta);
    state.twist.linear.x = a->getvx();
    state.twist.linear.y = a->getvy();
    state.twist.linear.z = a->getvz();

    // State machine behaviour
    AgentStateMachine::AgentState sc = a->getStateMachine()->getCurrentState();
    state.social_state = agentStateToActivity(sc);
    if (a->getType() == Ped::Tagent::ELDER) {
      state.social_state = AgentState::TYPE_STANDING;
    }

    // Forces.
    AgentForce agent_forces;
    agent_forces.desired_force = VecToMsg(a->getDesiredDirection());
    agent_forces.obstacle_force = VecToMsg(a->getObstacleForce());
    agent_forces.social_force = VecToMsg(a->getSocialForce());
    // agent_forces.group_coherence_force = a->getSocialForce();
    // agent_forces.group_gaze_force = a->getSocialForce();
    // agent_forces.group_repulsion_force = a->getSocialForce();
    // agent_forces.random_force = a->getSocialForce();

    state.forces = agent_forces;

    // Skip robot.
    if (a->getType() == Ped::Tagent::ROBOT) {
      robot_eval = agent_eval;
      continue;
    }
    else {
      agents_eval.agents.push_back(agent_eval);
    }

    all_status.agent_states.push_back(state);

  }

  pub_agent_states_->publish(all_status);
  pub_robot_state_->publish(robot_eval);
  pub_human_states_->publish(agents_eval);
}

void Simulator::publishGroups() {
  if (!groups_enabled) {
    RCLCPP_DEBUG_STREAM(get_logger(),
                        "Groups are disabled, no group data published: flag="
                            << groups_enabled);
    return;
  }

  if (SCENE.getGroups().size() < 1) {
    return;
  }

  AgentGroups sim_groups;
  sim_groups.header = createMsgHeader();

  for (const auto &ped_group : SCENE.getGroups()) {
    if (ped_group->memberCount() <= 1)
      continue;

    pedsim_msgs::msg::AgentGroup group;
    group.group_id = ped_group->getId();
    group.age = 10;
    const Ped::Tvector com = ped_group->getCenterOfMass();
    group.center_of_mass.position.x = com.x;
    group.center_of_mass.position.y = com.y;

    for (const auto &member : ped_group->getMembers()) {
      group.members.emplace_back(member->getId());
    }
    sim_groups.groups.emplace_back(group);
  }
  pub_agent_groups_->publish(sim_groups);
}

void Simulator::publishObstacles() {
  LineObstacles sim_obstacles;
  sim_obstacles.header = createMsgHeader();
  for (const auto &obstacle : SCENE.getObstacles()) {
    LineObstacle line_obstacle;
    line_obstacle.start.x = obstacle->getax();
    line_obstacle.start.y = obstacle->getay();
    line_obstacle.start.z = 0.0;
    line_obstacle.end.x = obstacle->getbx();
    line_obstacle.end.y = obstacle->getby();
    line_obstacle.end.z = 0.0;
    sim_obstacles.obstacles.push_back(line_obstacle);
  }
  pub_obstacles_->publish(sim_obstacles);
}

std::string Simulator::agentStateToActivity(
    const AgentStateMachine::AgentState &state) const {
  std::string activity = "Unknown";
  switch (state) {
  case AgentStateMachine::AgentState::StateWalking:
    activity = AgentState::TYPE_INDIVIDUAL_MOVING;
    break;
  case AgentStateMachine::AgentState::StateGroupWalking:
    activity = AgentState::TYPE_GROUP_MOVING;
    break;
  case AgentStateMachine::AgentState::StateQueueing:
    activity = AgentState::TYPE_WAITING_IN_QUEUE;
    break;
  case AgentStateMachine::AgentState::StateShopping:
    break;
  case AgentStateMachine::AgentState::StateNone:
    break;
  case AgentStateMachine::AgentState::StateWaiting:
    break;
  }
  return activity;
}

std_msgs::msg::Header Simulator::createMsgHeader() const {
  std_msgs::msg::Header msg_header;
  msg_header.stamp = rclcpp::Clock().now();
  msg_header.frame_id = odom_frame_id;
  return msg_header;
}
