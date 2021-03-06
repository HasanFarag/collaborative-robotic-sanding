/*
 * @author ros-industrial
 * @file process_execution_manager.cpp
 * @date Jan 16, 2020
 * @copyright Copyright (c) 2020, Southwest Research Institute
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2020, Southwest Research Institute
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *       * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *       * Neither the name of the Southwest Research Institute, nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
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
 */

#include <boost/format.hpp>
#include <crs_motion_planning/path_processing_utils.h>
#include "crs_application/task_managers/process_execution_manager.h"

static const double WAIT_SERVER_TIMEOUT = 10.0;  // seconds
static const std::string MANAGER_NAME = "ProcessExecutionManager";
static const std::string FOLLOW_JOINT_TRAJECTORY_ACTION = "follow_joint_trajectory";

namespace crs_application
{
namespace task_managers
{
ProcessExecutionManager::ProcessExecutionManager(std::shared_ptr<rclcpp::Node> node) : node_(node)
{
  trajectory_exec_client_cbgroup_ =
      node_->create_callback_group(rclcpp::callback_group::CallbackGroupType::MutuallyExclusive);
  trajectory_exec_client_ =
      rclcpp_action::create_client<control_msgs::action::FollowJointTrajectory>(node_->get_node_base_interface(),
                                                                                node_->get_node_graph_interface(),
                                                                                node_->get_node_logging_interface(),
                                                                                node_->get_node_waitables_interface(),
                                                                                FOLLOW_JOINT_TRAJECTORY_ACTION,
                                                                                trajectory_exec_client_cbgroup_);
}

ProcessExecutionManager::~ProcessExecutionManager() {}
common::ActionResult ProcessExecutionManager::init()
{
  // waiting for server
  if (!trajectory_exec_client_->wait_for_action_server(std::chrono::duration<double>(WAIT_SERVER_TIMEOUT)))
  {
    RCLCPP_ERROR(node_->get_logger(),
                 "%s Failed to find action server %s",
                 MANAGER_NAME.c_str(),
                 FOLLOW_JOINT_TRAJECTORY_ACTION.c_str());
    return false;
  }
  return true;
}

common::ActionResult ProcessExecutionManager::configure(const config::ProcessExecutionConfig& config)
{
  config_ = std::make_shared<config::ProcessExecutionConfig>(config);
  return true;
}

common::ActionResult ProcessExecutionManager::setInput(const datatypes::ProcessExecutionData& input)
{
  input_ = std::make_shared<datatypes::ProcessExecutionData>(input);
  return true;
}

common::ActionResult ProcessExecutionManager::execProcess()
{
  const crs_msgs::msg::ProcessMotionPlan& process_plan = input_->process_plans[current_process_idx_];
  RCLCPP_INFO(node_->get_logger(), "%s Executing process %i", MANAGER_NAME.c_str(), current_process_idx_);
  if (!execTrajectory(process_plan.start))
  {
    common::ActionResult res;
    res.err_msg = boost::str(boost::format("%s failed to execute start motion") % MANAGER_NAME.c_str());
    RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
    res.succeeded = false;
    return res;
  }

  for (std::size_t i = 0; i < process_plan.process_motions.size(); i++)
  {
    RCLCPP_INFO(node_->get_logger(), "%s Executing process path %i", MANAGER_NAME.c_str(), i);
    if (!execTrajectory(process_plan.process_motions[i]))
    {
      common::ActionResult res;
      res.err_msg = boost::str(boost::format("%s failed to execute process motion %i") % MANAGER_NAME.c_str() % i);
      RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
      res.succeeded = false;
      return res;
    }

    if (i >= process_plan.free_motions.size())
    {
      RCLCPP_INFO(
          node_->get_logger(), "%s No free motion to follow process path %i, skipping", MANAGER_NAME.c_str(), i);
      continue;
    }

    RCLCPP_INFO(node_->get_logger(), "%s Executing free motion %i", MANAGER_NAME.c_str(), i);
    if (!execTrajectory(process_plan.free_motions[i]))
    {
      common::ActionResult res;
      res.err_msg = boost::str(boost::format("%s failed to execute free motion %i") % MANAGER_NAME.c_str() % i);
      RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
      res.succeeded = false;
      return res;
    }
  }

  RCLCPP_INFO(node_->get_logger(), "%s Executing end motion", MANAGER_NAME.c_str());
  if (!execTrajectory(process_plan.end))
  {
    common::ActionResult res;
    res.err_msg = boost::str(boost::format("%s failed to execute end motion") % MANAGER_NAME.c_str());
    RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
    res.succeeded = false;
    return res;
  }
  RCLCPP_INFO(node_->get_logger(), "%s Completed process %i", MANAGER_NAME.c_str(), current_process_idx_);

  return true;
}

common::ActionResult ProcessExecutionManager::execMediaChange()
{
  if (input_->media_change_plans.empty())
  {
    RCLCPP_WARN(node_->get_logger(), "No media change moves to execute, skipping");
    return true;
  }

  datatypes::MediaChangeMotionPlan& mc_motion_plan = input_->media_change_plans[current_media_change_idx_];
  RCLCPP_INFO(node_->get_logger(), "%s Executing media change %i", MANAGER_NAME.c_str(), current_media_change_idx_);
  if (!execTrajectory(mc_motion_plan.start_traj))
  {
    common::ActionResult res;
    res.err_msg = boost::str(boost::format("%s failed to execute media change motion") % MANAGER_NAME.c_str());
    RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
    res.succeeded = false;
    return res;
  }

  return true;
}

common::ActionResult ProcessExecutionManager::checkQueue()
{
  common::ActionResult res;
  if (current_process_idx_ >= input_->process_plans.size() - 1)
  {
    res.succeeded = true;
    res.opt_data = datatypes::ProcessExecActions::DONE;
    return res;
  }

  if (current_process_idx_ > current_media_change_idx_)
  {
    current_media_change_idx_ = current_process_idx_;
    if (current_media_change_idx_ < input_->media_change_plans.size())
    {
      res.succeeded = true;
      res.opt_data = datatypes::ProcessExecActions::EXEC_MEDIA_CHANGE;
      return res;
    }
  }

  current_process_idx_++;
  res.succeeded = true;
  res.opt_data = datatypes::ProcessExecActions::EXEC_PROCESS;
  return res;
}

common::ActionResult ProcessExecutionManager::execHome()
{
  RCLCPP_WARN(node_->get_logger(), "%s No home motion to execute", MANAGER_NAME.c_str());
  return true;
}

common::ActionResult ProcessExecutionManager::moveStart()
{
  // check input
  common::ActionResult res = checkPreReq();
  if (!res)
  {
    return res;
  }

  resetIndexes();

  if (input_->move_to_start.points.empty())
  {
    RCLCPP_WARN(node_->get_logger(), "%s No start trajectory was provided, skipping", MANAGER_NAME.c_str());
    return true;
  }

  RCLCPP_INFO(node_->get_logger(), "%s Executing start trajectory", MANAGER_NAME.c_str());
  res = execTrajectory(input_->move_to_start);
  return res;
}

common::ActionResult ProcessExecutionManager::execMoveReturn()
{
  datatypes::MediaChangeMotionPlan& mc_motion_plan = input_->media_change_plans[current_media_change_idx_];
  RCLCPP_INFO(node_->get_logger(),
              "%s Executing return move %i back to process ",
              MANAGER_NAME.c_str(),
              current_media_change_idx_);
  if (!execTrajectory(mc_motion_plan.return_traj))
  {
    common::ActionResult res;
    res.err_msg = boost::str(boost::format("%s failed to execute return move") % MANAGER_NAME.c_str());
    RCLCPP_ERROR(node_->get_logger(), "%s", res.err_msg.c_str());
    res.succeeded = false;
    return res;
  }

  return true;
}

common::ActionResult ProcessExecutionManager::cancelMotion()
{
  using GS = action_msgs::msg::GoalStatus;
  if (trajectory_exec_fut_.valid())
  {
    trajectory_exec_client_->async_cancel_all_goals();
    trajectory_exec_fut_ = std::shared_future<GoalHandleT::SharedPtr>();
  }
  return true;
}

void ProcessExecutionManager::resetIndexes()
{
  current_process_idx_ = 0;
  current_media_change_idx_ = -1;
}

common::ActionResult ProcessExecutionManager::execTrajectory(const trajectory_msgs::msg::JointTrajectory& traj)
{
  using namespace control_msgs::action;
  static const double GOAL_ACCEPT_TIMEOUT_PERIOD = 1.0;

  // rclcpp::Duration traj_dur(traj.points.back().time_from_start);
  common::ActionResult res = false;

  res.succeeded = crs_motion_planning::execTrajectory(trajectory_exec_client_, node_->get_logger(), traj);
  if (!res)
  {
    return res;
  }
  /*

    FollowJointTrajectory::Goal goal;
    goal.trajectory = traj;
    auto goal_options = rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions();
    // TODO populate tolerances

    // send goal
    trajectory_exec_fut_ = trajectory_exec_client_->async_send_goal(goal);
    traj_dur = traj_dur + rclcpp::Duration(config_->traj_time_tolerance);

    // wait for goal
    if (trajectory_exec_fut_.wait_for(std::chrono::duration<double>(GOAL_ACCEPT_TIMEOUT_PERIOD)) !=
        std::future_status::ready)
    {
      res.err_msg = "Timed out waiting for goal to be accepted";
      RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
      trajectory_exec_client_->async_cancel_all_goals();
      return res;
    }

    // get goal handle
    auto gh = trajectory_exec_fut_.get();
    if (!gh)
    {
      res.err_msg = "Goal was rejected";
      RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
      trajectory_exec_client_->async_cancel_all_goals();
      return res;
    }

    // wait for result
    if (trajectory_exec_client_->async_get_result(gh).wait_for(traj_dur.to_chrono<std::chrono::seconds>()) !=
        std::future_status::ready)
    {
      res.err_msg = "Trajectory execution timed out";
      RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
      trajectory_exec_client_->async_cancel_all_goals();
      return res;
    }

    rclcpp_action::ClientGoalHandle<FollowJointTrajectory>::WrappedResult wrapped_result =
        trajectory_exec_client_->async_get_result(gh).get();
    if (wrapped_result.code != rclcpp_action::ResultCode::SUCCEEDED)
    {
      res.err_msg = wrapped_result.result->error_string;
      RCLCPP_ERROR(node_->get_logger(), "Trajectory execution failed with error message: %s", res.err_msg.c_str());
      return res;
    }

    // reset future
    trajectory_exec_fut_ = std::shared_future<GoalHandleT::SharedPtr>();
    RCLCPP_INFO(node_->get_logger(), "%s Trajectory completed", MANAGER_NAME.c_str());*/
  return true;
}

common::ActionResult ProcessExecutionManager::checkPreReq()
{
  if (!config_)
  {
    common::ActionResult res = { succeeded : false, err_msg : "No configuration has been provided, can not proceed" };
    RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
    return res;
  }

  if (!input_)
  {
    common::ActionResult res = { succeeded : false, err_msg : "No input data has been provided, can not proceed" };
    RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
    return res;
  }

  if (input_->process_plans.empty())
  {
    common::ActionResult res;
    res.succeeded = false;
    res.err_msg = "Process plans buffer is empty";
    RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
    return res;
  }

  if (!trajectory_exec_client_->action_server_is_ready())
  {
    common::ActionResult res;
    res.succeeded = false;
    res.err_msg = "No trajectory execution servers were found";
    RCLCPP_ERROR(node_->get_logger(), "%s %s", MANAGER_NAME.c_str(), res.err_msg.c_str());
    return res;
  }
  return true;
}

} /* namespace task_managers */
} /* namespace crs_application */
