//
// Created by rgrandia on 30.04.21.
//

#include "ocs2_switched_model_interface/cost/MotionTrackingCost.h"

#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <ocs2_core/misc/LoadData.h>

#include "ocs2_switched_model_interface/core/Rotations.h"

namespace switched_model {

namespace {
constexpr size_t baseTargets = 12;
constexpr size_t legTargets = 12;
constexpr size_t costVectorLength = baseTargets + NUM_CONTACT_POINTS * legTargets;

template <typename SCALAR_T>
struct CostElements {
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  vector3_s_t<SCALAR_T> eulerXYZ{vector3_s_t<SCALAR_T>::Zero()};
  vector3_s_t<SCALAR_T> comPosition{vector3_s_t<SCALAR_T>::Zero()};
  vector3_s_t<SCALAR_T> comAngularVelocity{vector3_s_t<SCALAR_T>::Zero()};
  vector3_s_t<SCALAR_T> comLinearVelocity{vector3_s_t<SCALAR_T>::Zero()};
  feet_array_t<vector3_s_t<SCALAR_T>> jointPosition{constantFeetArray<vector3_s_t<SCALAR_T>>(vector3_s_t<SCALAR_T>::Zero())};
  feet_array_t<vector3_s_t<SCALAR_T>> footPosition{constantFeetArray<vector3_s_t<SCALAR_T>>(vector3_s_t<SCALAR_T>::Zero())};
  feet_array_t<vector3_s_t<SCALAR_T>> footVelocity{constantFeetArray<vector3_s_t<SCALAR_T>>(vector3_s_t<SCALAR_T>::Zero())};
  feet_array_t<vector3_s_t<SCALAR_T>> contactForce{constantFeetArray<vector3_s_t<SCALAR_T>>(vector3_s_t<SCALAR_T>::Zero())};
};

template <typename SCALAR_T>
Eigen::Matrix<SCALAR_T, -1, 1> costElementsToVector(const CostElements<SCALAR_T>& asStruct) {
  Eigen::Matrix<SCALAR_T, -1, 1> v(costVectorLength);

  // Base
  v.head(baseTargets) << asStruct.eulerXYZ, asStruct.comPosition, asStruct.comAngularVelocity, asStruct.comLinearVelocity;

  // Legs
  for (int leg = 0; leg < NUM_CONTACT_POINTS; ++leg) {
    v.segment(baseTargets + leg * legTargets, legTargets) << asStruct.jointPosition[leg], asStruct.footPosition[leg],
        asStruct.footVelocity[leg], asStruct.contactForce[leg];
  }
  return v;
}

template <typename SCALAR_T>
Eigen::Matrix<SCALAR_T, -1, 1> computeMotionTargets(const comkino_state_s_t<SCALAR_T>& x, const comkino_input_s_t<SCALAR_T>& u,
                                                    const KinematicsModelBase<SCALAR_T>& kinematics) {
  // Extract elements from reference
  const auto comPose = getComPose(x);
  const auto com_comTwist = getComLocalVelocities(x);
  const auto o_R_b = rotationMatrixBaseToOrigin(getOrientation(comPose));
  const auto qJoints = getJointPositions(x);
  const auto dqJoints = getJointVelocities(u);

  CostElements<SCALAR_T> motionTarget;
  motionTarget.eulerXYZ = getOrientation(comPose);
  motionTarget.comPosition = getPositionInOrigin(comPose);
  motionTarget.comAngularVelocity = o_R_b * getAngularVelocity(com_comTwist);
  motionTarget.comLinearVelocity = o_R_b * getLinearVelocity(com_comTwist);
  for (size_t leg = 0; leg < NUM_CONTACT_POINTS; ++leg) {
    motionTarget.jointPosition[leg] = qJoints.template segment<3>(3 * leg);
    motionTarget.footPosition[leg] = kinematics.positionBaseToFootInBaseFrame(leg, qJoints);
    motionTarget.footVelocity[leg] = kinematics.footVelocityRelativeToBaseInBaseFrame(leg, qJoints, dqJoints);
    motionTarget.contactForce[leg] = u.template segment<3>(3 * leg);
  }
  return costElementsToVector(motionTarget);
}

}  // namespace

MotionTrackingCost::MotionTrackingCost(const Weights& settings, const SwitchedModelModeScheduleManager& modeScheduleManager,
                                       const kinematic_model_t& kinematicModel, const ad_kinematic_model_t& adKinematicModel,
                                       const com_model_t& comModel, bool recompile)
    : modeScheduleManagerPtr_(&modeScheduleManager),
      kinematicModelPtr_(kinematicModel.clone()),
      adKinematicModelPtr_(adKinematicModel.clone()),
      comModelPtr_(comModel.clone()) {
  // Weights are sqrt of settings
  CostElements<ocs2::ad_scalar_t> weightStruct;
  weightStruct.eulerXYZ = settings.eulerXYZ.cwiseSqrt().cast<ocs2::ad_scalar_t>();
  weightStruct.comPosition = settings.comPosition.cwiseSqrt().cast<ocs2::ad_scalar_t>();
  weightStruct.comAngularVelocity = settings.comAngularVelocity.cwiseSqrt().cast<ocs2::ad_scalar_t>();
  weightStruct.comLinearVelocity = settings.comLinearVelocity.cwiseSqrt().cast<ocs2::ad_scalar_t>();
  for (size_t leg = 0; leg < NUM_CONTACT_POINTS; ++leg) {
    weightStruct.jointPosition[leg] = settings.jointPosition.cwiseSqrt().cast<ocs2::ad_scalar_t>();
    weightStruct.footPosition[leg] = settings.footPosition.cwiseSqrt().cast<ocs2::ad_scalar_t>();
    weightStruct.footVelocity[leg] = settings.footVelocity.cwiseSqrt().cast<ocs2::ad_scalar_t>();
    weightStruct.contactForce[leg] = settings.contactForce.cwiseSqrt().cast<ocs2::ad_scalar_t>();
  }
  sqrtWeights_ = costElementsToVector(weightStruct);

  initialize(STATE_DIM, INPUT_DIM, costVectorLength, "MotionTrackingCost", "/tmp/ocs2", recompile);
};

ocs2::vector_t MotionTrackingCost::getParameters(ocs2::scalar_t time, const ocs2::CostDesiredTrajectories& desiredTrajectory) const {
  // Interpolate reference
  const comkino_state_t xRef = desiredTrajectory.getDesiredState(time);
  comkino_input_t uRef = desiredTrajectory.getDesiredInput(time);

  // If the input has zero values, overwrite it.
  if (uRef.isZero()) {
    // Get stance configuration
    const auto contactFlags = modeScheduleManagerPtr_->getContactFlags(time);
    uRef = weightCompensatingInputs(*comModelPtr_, contactFlags, getOrientation(getComPose(xRef)));
  }

  // The target references are the parameters
  return computeMotionTargets<ocs2::scalar_t>(xRef, uRef, *kinematicModelPtr_);
}

MotionTrackingCost::MotionTrackingCost(const MotionTrackingCost& other)
    : ocs2::StateInputCostGaussNewtonAd(other),
      sqrtWeights_(other.sqrtWeights_),
      modeScheduleManagerPtr_(other.modeScheduleManagerPtr_),
      kinematicModelPtr_(other.kinematicModelPtr_->clone()),
      adKinematicModelPtr_(other.adKinematicModelPtr_->clone()),
      comModelPtr_(other.comModelPtr_->clone()) {}

ocs2::ad_vector_t MotionTrackingCost::costVectorFunction(ocs2::ad_scalar_t time, const ocs2::ad_vector_t& state,
                                                         const ocs2::ad_vector_t& input, const ocs2::ad_vector_t& parameters) const {
  const auto currentTargets = computeMotionTargets<ocs2::ad_scalar_t>(state, input, *adKinematicModelPtr_);
  return (currentTargets - parameters).cwiseProduct(sqrtWeights_);
}

MotionTrackingCost::Weights loadWeightsFromFile(const std::string& filename, const std::string& fieldname, bool verbose) {
  MotionTrackingCost::Weights weights;

  boost::property_tree::ptree pt;
  boost::property_tree::read_info(filename, pt);

  if (verbose) {
    std::cerr << "\n #### Tacking Cost Weights:" << std::endl;
    std::cerr << " #### ==================================================" << std::endl;
  }

  ocs2::loadData::loadPtreeValue(pt, weights.eulerXYZ.x(), fieldname + ".roll", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.eulerXYZ.y(), fieldname + ".pitch", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.eulerXYZ.z(), fieldname + ".yaw", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comPosition.x(), fieldname + ".base_position_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comPosition.y(), fieldname + ".base_position_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comPosition.z(), fieldname + ".base_position_z", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comAngularVelocity.x(), fieldname + ".base_angular_vel_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comAngularVelocity.y(), fieldname + ".base_angular_vel_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comAngularVelocity.z(), fieldname + ".base_angular_vel_z", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comLinearVelocity.x(), fieldname + ".base_linear_vel_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comLinearVelocity.y(), fieldname + ".base_linear_vel_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.comLinearVelocity.z(), fieldname + ".base_linear_vel_z", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.jointPosition.x(), fieldname + ".joint_position_HAA", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.jointPosition.y(), fieldname + ".joint_position_HFE", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.jointPosition.z(), fieldname + ".joint_position_KFE", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footPosition.x(), fieldname + ".foot_position_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footPosition.y(), fieldname + ".foot_position_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footPosition.z(), fieldname + ".foot_position_z", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footVelocity.x(), fieldname + ".foot_velocity_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footVelocity.y(), fieldname + ".foot_velocity_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.footVelocity.z(), fieldname + ".foot_velocity_z", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.contactForce.x(), fieldname + ".contact_force_x", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.contactForce.y(), fieldname + ".contact_force_y", verbose);
  ocs2::loadData::loadPtreeValue(pt, weights.contactForce.z(), fieldname + ".contact_force_z", verbose);

  if (verbose) {
    std::cerr << " #### ================================================ ####" << std::endl;
  }

  return weights;
}

}  // namespace switched_model