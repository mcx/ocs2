#include "ocs2_switched_model_interface/constraint/ComKinoConstraintBaseAd.h"

// Constraints
#include "ocs2_switched_model_interface/constraint/EndEffectorVelocityContraint.h"
#include "ocs2_switched_model_interface/constraint/FrictionConeConstraint.h"
#include "ocs2_switched_model_interface/constraint/ZeroForceConstraint.h"

namespace switched_model {

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ComKinoConstraintBaseAd* ComKinoConstraintBaseAd::clone() const {
  return new ComKinoConstraintBaseAd(*this);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::InitializeConstraintTerms() {

  for (int i = 0; i < NUM_CONTACT_POINTS; i++) {
    auto footName = feetNames[i];

    // Friction cone constraint
    auto frictionCone = std::unique_ptr<ConstraintTerm_t>(new FrictionConeConstraint(options_.frictionCoefficient_, 25.0, i));

    // EE force
    auto zeroForceConstraint = std::unique_ptr<ConstraintTerm_t>(new ZeroForceConstraint(i));

    // Velocity Constraint

    auto endEffectorVelocityConstraint = std::unique_ptr<ConstraintTerm_t>(new EndEffectorVelocityConstraint(
        i, EndEffectorVelocityConstraintSettings(), *adComModelPtr_.get(), *adKinematicModelPtr_.get(), options_.recompileLibraries_));

    // Inequalities
    inequalityConstraintCollection_.add(std::move(frictionCone), footName + "_FrictionCone");

    // State input equalities
    equalityStateInputConstraintCollection_.add(std::move(zeroForceConstraint), footName + "_ZeroForce");
    equalityStateInputConstraintCollection_.add(std::move(endEffectorVelocityConstraint), footName + "_EEVel");
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::setCurrentStateAndControl(const scalar_t& t, const state_vector_t& x, const input_vector_t& u) {
  stateInputConstraintsComputed_ = false;
  inequalityConstraintsComputed_ = false;

  Base::setCurrentStateAndControl(t, x, u);
  numEventTimes_ = logicRulesPtr_->getNumEventTimes();
  auto activeSubsystem = logicRulesPtr_->getEventTimeCount(t);
  logicRulesPtr_->getMotionPhaseLogics(activeSubsystem, stanceLegs_, zDirectionRefsPtr_);

  for (int i = 0; i < NUM_CONTACT_POINTS; i++) {
    auto footName = feetNames[i];

    // Active friction cone constraint for stanceLegs
    inequalityConstraintCollection_.modifyConstraint(footName + "_FrictionCone")->setActivity(stanceLegs_[i]);

    // Zero forces active for swing legs
    equalityStateInputConstraintCollection_.modifyConstraint(footName + "_ZeroForce")->setActivity(!stanceLegs_[i]);

    // Active foot placement for stance legs
    auto EEVelConstraint =
        equalityStateInputConstraintCollection_.template modifyConstraint<EndEffectorVelocityConstraint>(footName + "_EEVel");
    EEVelConstraint->setActivity(true);
    EndEffectorVelocityConstraintSettings eeVelConSettings;
    if (stanceLegs_[i]) {  // in stance: All velocity equal to zero
      eeVelConSettings.b = Eigen::Vector3d::Zero();
      eeVelConSettings.A = Eigen::Matrix3d::Identity();
    } else {  // in swing: z-velocity is provided
      eeVelConSettings.b.resize(1);
      eeVelConSettings.A.resize(1, 3);
      eeVelConSettings.b << -zDirectionRefsPtr_[i]->calculateVelocity(Base::t_);
      eeVelConSettings.A << 0, 0, 1;
    }
    EEVelConstraint->configure(eeVelConSettings);
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint1(constraint1_vector_t& g1) {
  size_t numConstraints = numStateInputConstraint(Base::t_);
  g1.head(numConstraints) = equalityStateInputConstraintCollection_.getConstraints().getValueAsVector(Base::t_, Base::x_, Base::u_);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
size_t ComKinoConstraintBaseAd::numStateInputConstraint(const scalar_t& time) {
  return equalityStateInputConstraintCollection_.getConstraints().getNumConstraints(time);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint2(constraint2_vector_t& g2) {}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
size_t ComKinoConstraintBaseAd::numStateOnlyConstraint(const scalar_t& time) {
  return 0;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraint(scalar_array_t& h) {
  h = inequalityConstraintCollection_.getConstraints().getValue(Base::t_, Base::x_, Base::u_);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
size_t ComKinoConstraintBaseAd::numInequalityConstraint(const scalar_t& time) {
  return inequalityConstraintCollection_.getConstraints().getNumConstraints(time);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getFinalConstraint2(constraint2_vector_t& g2Final) {}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
size_t ComKinoConstraintBaseAd::numStateOnlyFinalConstraint(const scalar_t& time) {
  return 0;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint1DerivativesState(constraint1_state_matrix_t& C) {
  if (!stateInputConstraintsComputed_) {
    linearStateInputConstraintApproximation_ =
        equalityStateInputConstraintCollection_.getConstraints().getLinearApproximationAsMatrices(Base::t_, Base::x_, Base::u_);
    stateInputConstraintsComputed_ = true;
  }
  size_t numConstraints = numStateInputConstraint(Base::t_);
  C.topRows(numConstraints) = linearStateInputConstraintApproximation_.derivativeState.topRows(numConstraints);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint1DerivativesControl(constraint1_input_matrix_t& D) {
  if (!stateInputConstraintsComputed_) {
    linearStateInputConstraintApproximation_ =
        equalityStateInputConstraintCollection_.getConstraints().getLinearApproximationAsMatrices(Base::t_, Base::x_, Base::u_);
    stateInputConstraintsComputed_ = true;
  }
  size_t numConstraints = numStateInputConstraint(Base::t_);
  D.topRows(numConstraints) = linearStateInputConstraintApproximation_.derivativeInput.topRows(numConstraints);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint1DerivativesEventTimes(constraint1_vector_array_t& g1DevArray) {
  // set all to zero
  g1DevArray.resize(numEventTimes_);
  for (constraint1_vector_t& g1Dev : g1DevArray) g1Dev.setZero();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getConstraint2DerivativesState(constraint2_state_matrix_t& F) {}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraintDerivativesState(state_vector_array_t& dhdx) {
  if (!inequalityConstraintsComputed_) {
    quadraticInequalityConstraintApproximation_ =
        inequalityConstraintCollection_.getConstraints().getQuadraticApproximation(Base::t_, Base::x_, Base::u_);
    inequalityConstraintsComputed_ = true;
  }
  dhdx = std::move(quadraticInequalityConstraintApproximation_.derivativeState);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraintDerivativesInput(switched_model::ComKinoConstraintBaseAd::input_vector_array_t& dhdu) {
  if (!inequalityConstraintsComputed_) {
    quadraticInequalityConstraintApproximation_ =
        inequalityConstraintCollection_.getConstraints().getQuadraticApproximation(Base::t_, Base::x_, Base::u_);
    inequalityConstraintsComputed_ = true;
  }
  dhdu = std::move(quadraticInequalityConstraintApproximation_.derivativeInput);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraintSecondDerivativesState(
    switched_model::ComKinoConstraintBaseAd::state_matrix_array_t& ddhdxdx) {
  if (!inequalityConstraintsComputed_) {
    quadraticInequalityConstraintApproximation_ =
        inequalityConstraintCollection_.getConstraints().getQuadraticApproximation(Base::t_, Base::x_, Base::u_);
    inequalityConstraintsComputed_ = true;
  }
  ddhdxdx = std::move(quadraticInequalityConstraintApproximation_.secondDerivativesState);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraintSecondDerivativesInput(
    switched_model::ComKinoConstraintBaseAd::input_matrix_array_t& ddhdudu) {
  if (!inequalityConstraintsComputed_) {
    quadraticInequalityConstraintApproximation_ =
        inequalityConstraintCollection_.getConstraints().getQuadraticApproximation(Base::t_, Base::x_, Base::u_);
    inequalityConstraintsComputed_ = true;
  }
  ddhdudu = std::move(quadraticInequalityConstraintApproximation_.secondDerivativesInput);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getInequalityConstraintDerivativesInputState(
    switched_model::ComKinoConstraintBaseAd::input_state_matrix_array_t& ddhdudx) {
  if (!inequalityConstraintsComputed_) {
    quadraticInequalityConstraintApproximation_ =
        inequalityConstraintCollection_.getConstraints().getQuadraticApproximation(Base::t_, Base::x_, Base::u_);
    inequalityConstraintsComputed_ = true;
  }
  ddhdudx = std::move(quadraticInequalityConstraintApproximation_.derivativesInputState);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getFinalConstraint2DerivativesState(constraint2_state_matrix_t& F) {}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::setStanceLegs(const contact_flag_t& stanceLegs) {
  stanceLegs_ = stanceLegs;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ComKinoConstraintBaseAd::getStanceLegs(contact_flag_t& stanceLegs) {
  stanceLegs = stanceLegs_;
}

}  // end of namespace switched_model
