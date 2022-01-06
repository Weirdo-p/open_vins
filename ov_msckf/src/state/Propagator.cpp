/*
 * OpenVINS: An Open Platform for Visual-Inertial Research
 * Copyright (C) 2021 Patrick Geneva
 * Copyright (C) 2021 Guoquan Huang
 * Copyright (C) 2021 OpenVINS Contributors
 * Copyright (C) 2019 Kevin Eckenhoff
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Propagator.h"

using namespace ov_core;
using namespace ov_type;
using namespace ov_msckf;

void Propagator::propagate_and_clone(std::shared_ptr<State> state, double timestamp) {

  // If the difference between the current update time and state is zero
  // We should crash, as this means we would have two clones at the same time!!!!
  if (state->_timestamp == timestamp) {
    PRINT_ERROR(RED "Propagator::propagate_and_clone(): Propagation called again at same timestep at last update timestep!!!!\n" RESET);
    std::exit(EXIT_FAILURE);
  }

  // We should crash if we are trying to propagate backwards
  if (state->_timestamp > timestamp) {
    PRINT_ERROR(RED "Propagator::propagate_and_clone(): Propagation called trying to propagate backwards in time!!!!\n" RESET);
    PRINT_ERROR(RED "Propagator::propagate_and_clone(): desired propagation = %.4f\n" RESET, (timestamp - state->_timestamp));
    std::exit(EXIT_FAILURE);
  }

  //===================================================================================
  //===================================================================================
  //===================================================================================

  // Set the last time offset value if we have just started the system up
  if (!have_last_prop_time_offset) {
    last_prop_time_offset = state->_calib_dt_CAMtoIMU->value()(0);
    have_last_prop_time_offset = true;
  }

  // Get what our IMU-camera offset should be (t_imu = t_cam + calib_dt)
  double t_off_new = state->_calib_dt_CAMtoIMU->value()(0);

  // First lets construct an IMU vector of measurements we need
  double time0 = state->_timestamp + last_prop_time_offset;
  double time1 = timestamp + t_off_new;
  std::vector<ov_core::ImuData> prop_data = Propagator::select_imu_readings(imu_data, time0, time1);

  // We are going to sum up all the state transition matrices, so we can do a single large multiplication at the end
  // Phi_summed = Phi_i*Phi_summed
  // Q_summed = Phi_i*Q_summed*Phi_i^T + Q_i
  // After summing we can multiple the total phi to get the updated covariance
  // We will then add the noise to the IMU portion of the state
  Eigen::MatrixXd Phi_summed = Eigen::MatrixXd::Identity(state->imu_intrinsic_size() + 15, state->imu_intrinsic_size() + 15);
  Eigen::MatrixXd Qd_summed = Eigen::MatrixXd::Zero(state->imu_intrinsic_size() + 15, state->imu_intrinsic_size() + 15);
  double dt_summed = 0;

  // Loop through all IMU messages, and use them to move the state forward in time
  // This uses the zero'th order quat, and then constant acceleration discrete
  if (prop_data.size() > 1) {
    for (size_t i = 0; i < prop_data.size() - 1; i++) {

      // Get the next state Jacobian and noise Jacobian for this IMU reading
      Eigen::MatrixXd F, Qdi;
      predict_and_compute(state, prop_data.at(i), prop_data.at(i + 1), F, Qdi);

      // Next we should propagate our IMU covariance
      // Pii' = F*Pii*F.transpose() + G*Q*G.transpose()
      // Pci' = F*Pci and Pic' = Pic*F.transpose()
      // NOTE: Here we are summing the state transition F so we can do a single mutiplication later
      // NOTE: Phi_summed = Phi_i*Phi_summed
      // NOTE: Q_summed = Phi_i*Q_summed*Phi_i^T + G*Q_i*G^T
      Phi_summed = F * Phi_summed;
      Qd_summed = F * Qd_summed * F.transpose() + Qdi;
      Qd_summed = 0.5 * (Qd_summed + Qd_summed.transpose());
      dt_summed += prop_data.at(i + 1).timestamp - prop_data.at(i).timestamp;
    }
  }

  // Last angular velocity (used for cloning when estimating time offset)
  // Remember to correct them before we store them
  Eigen::Vector3d last_a = Eigen::Vector3d::Zero();
  Eigen::Vector3d last_w = Eigen::Vector3d::Zero();
  if (!prop_data.empty()) {
    last_a = state->R_AcctoI() * state->Da() * (prop_data.at(prop_data.size() - 1).am - state->_imu->bias_a());
    // TODO: is this acceleration used correct?
    // Eigen::Vector3d last_aIiinG = state->_imu->Rot().transpose() * last_a - _gravity;
    last_w = state->R_GyrotoI() * state->Dw() * (prop_data.at(prop_data.size() - 1).wm - state->_imu->bias_g() - state->Tg() * last_a);
  }

  // Do the update to the covariance with our "summed" state transition and IMU noise addition...
  std::vector<std::shared_ptr<Type>> Phi_order;
  Phi_order.push_back(state->_imu);
  if (state->_options.do_calib_imu_intrinsics) {
    Phi_order.push_back(state->_calib_imu_dw);
    Phi_order.push_back(state->_calib_imu_da);
    if (state->_options.do_calib_imu_g_sensitivity) {
      Phi_order.push_back(state->_calib_imu_tg);
    }
    if (state->_options.imu_model == 0) {
      Phi_order.push_back(state->_calib_imu_GYROtoIMU);
    } else {
      Phi_order.push_back(state->_calib_imu_ACCtoIMU);
    }
  }
  StateHelper::EKFPropagation(state, Phi_order, Phi_order, Phi_summed, Qd_summed);

  // Set timestamp data
  state->_timestamp = timestamp;
  last_prop_time_offset = t_off_new;

  // Now perform stochastic cloning
  StateHelper::augment_clone(state, last_w);
}

void Propagator::fast_state_propagate(std::shared_ptr<State> state, double timestamp, Eigen::Matrix<double, 13, 1> &state_plus) {

  // Set the last time offset value if we have just started the system up
  if (!have_last_prop_time_offset) {
    last_prop_time_offset = state->_calib_dt_CAMtoIMU->value()(0);
    have_last_prop_time_offset = true;
  }

  // Get what our IMU-camera offset should be (t_imu = t_cam + calib_dt)
  double t_off_new = state->_calib_dt_CAMtoIMU->value()(0);

  // First lets construct an IMU vector of measurements we need
  double time0 = state->_timestamp + last_prop_time_offset;
  double time1 = timestamp + t_off_new;
  std::vector<ov_core::ImuData> prop_data = Propagator::select_imu_readings(imu_data, time0, time1, false);

  // Save the original IMU state
  Eigen::VectorXd orig_val = state->_imu->value();
  Eigen::VectorXd orig_fej = state->_imu->fej();

  // Loop through all IMU messages, and use them to move the state forward in time
  // This uses the zero'th order quat, and then constant acceleration discrete
  if (prop_data.size() > 1) {
    for (size_t i = 0; i < prop_data.size() - 1; i++) {

      // Time elapsed over interval
      auto data_plus = prop_data.at(i + 1);
      auto data_minus = prop_data.at(i);
      double dt = data_plus.timestamp - data_minus.timestamp;
      // assert(data_plus.timestamp>data_minus.timestamp);

      // Corrected imu acc measurements with our current biases
      Eigen::Vector3d a_hat1 = data_minus.am - state->_imu->bias_a();
      Eigen::Vector3d a_hat2 = data_plus.am - state->_imu->bias_a();
      Eigen::Vector3d a_hat = a_hat1;
      if (state->_options.imu_avg) {
        a_hat = .5 * (a_hat1 + a_hat2);
      }

      // Correct imu readings with IMU intrinsics
      a_hat = state->R_AcctoI() * state->Da() * a_hat;
      a_hat1 = state->R_AcctoI() * state->Da() * a_hat1;
      a_hat2 = state->R_AcctoI() * state->Da() * a_hat2;

      // Corrected imu gyro measurements with our current biases
      Eigen::Vector3d gravity_correction1 = state->Tg() * a_hat1;
      Eigen::Vector3d gravity_correction2 = state->Tg() * a_hat2;
      Eigen::Vector3d w_hat1 = data_minus.wm - state->_imu->bias_g() - gravity_correction1;
      Eigen::Vector3d w_hat2 = data_plus.wm - state->_imu->bias_g() - gravity_correction2;
      Eigen::Vector3d w_hat = w_hat1;
      if (state->_options.imu_avg) {
        w_hat = .5 * (w_hat1 + w_hat2);
      }

      // Correct imu readings with IMU intrinsics
      w_hat = state->R_GyrotoI() * state->Dw() * w_hat;
      w_hat1 = state->R_GyrotoI() * state->Dw() * w_hat1;
      w_hat2 = state->R_GyrotoI() * state->Dw() * w_hat2;

      // Pre-compute some analytical values
      Eigen::Matrix<double, 3, 18> Xi_sum = Eigen::Matrix<double, 3, 18>::Zero(3, 18);
      if (state->_options.use_analytic_integration) {
        compute_Xi_sum(state, dt, w_hat, a_hat, Xi_sum);
      }

      // Compute the new state mean value
      Eigen::Vector4d new_q;
      Eigen::Vector3d new_v, new_p;
      if (state->_options.use_analytic_integration) {
        predict_mean_analytic(state, dt, w_hat, a_hat, new_q, new_v, new_p, Xi_sum);
      } else if (state->_options.use_rk4_integration) {
        predict_mean_rk4(state, dt, w_hat, a_hat, w_hat2, a_hat2, new_q, new_v, new_p);
      } else {
        predict_mean_discrete(state, dt, w_hat, a_hat, w_hat2, a_hat2, new_q, new_v, new_p);
      }

      // Now replace imu estimate and fej with propagated values
      Eigen::Matrix<double, 16, 1> imu_x = state->_imu->value();
      imu_x.block(0, 0, 4, 1) = new_q;
      imu_x.block(4, 0, 3, 1) = new_p;
      imu_x.block(7, 0, 3, 1) = new_v;
      state->_imu->set_value(imu_x);
      state->_imu->set_fej(imu_x);
    }
  }

  // Now record what the predicted state should be
  // TODO: apply IMU intrinsics here to correct the angular velocity
  state_plus = Eigen::Matrix<double, 13, 1>::Zero();
  state_plus.block(0, 0, 4, 1) = state->_imu->quat();
  state_plus.block(4, 0, 3, 1) = state->_imu->pos();
  state_plus.block(7, 0, 3, 1) = state->_imu->vel();
  if (prop_data.size() > 1) {
    state_plus.block(10, 0, 3, 1) = prop_data.at(prop_data.size() - 2).wm - state->_imu->bias_g();
  } else if (!prop_data.empty()) {
    state_plus.block(10, 0, 3, 1) = prop_data.at(prop_data.size() - 1).wm - state->_imu->bias_g();
  }

  // Finally replace the imu with the original state we had
  state->_imu->set_value(orig_val);
  state->_imu->set_fej(orig_fej);
}

std::vector<ov_core::ImuData> Propagator::select_imu_readings(const std::vector<ov_core::ImuData> &imu_data, double time0, double time1,
                                                              bool warn) {

  // Our vector imu readings
  std::vector<ov_core::ImuData> prop_data;

  // Ensure we have some measurements in the first place!
  if (imu_data.empty()) {
    if (warn)
      PRINT_WARNING(YELLOW "Propagator::select_imu_readings(): No IMU measurements. IMU-CAMERA are likely messed up!!!\n" RESET);
    return prop_data;
  }

  // Loop through and find all the needed measurements to propagate with
  // Note we split measurements based on the given state time, and the update timestamp
  for (size_t i = 0; i < imu_data.size() - 1; i++) {

    // START OF THE INTEGRATION PERIOD
    // If the next timestamp is greater then our current state time
    // And the current is not greater then it yet...
    // Then we should "split" our current IMU measurement
    if (imu_data.at(i + 1).timestamp > time0 && imu_data.at(i).timestamp < time0) {
      ov_core::ImuData data = Propagator::interpolate_data(imu_data.at(i), imu_data.at(i + 1), time0);
      prop_data.push_back(data);
      // PRINT_DEBUG("propagation #%d = CASE 1 = %.3f => %.3f\n",
      // (int)i,data.timestamp-prop_data.at(0).timestamp,time0-prop_data.at(0).timestamp);
      continue;
    }

    // MIDDLE OF INTEGRATION PERIOD
    // If our imu measurement is right in the middle of our propagation period
    // Then we should just append the whole measurement time to our propagation vector
    if (imu_data.at(i).timestamp >= time0 && imu_data.at(i + 1).timestamp <= time1) {
      prop_data.push_back(imu_data.at(i));
      // PRINT_DEBUG("propagation #%d = CASE 2 = %.3f\n",(int)i,imu_data.at(i).timestamp-prop_data.at(0).timestamp);
      continue;
    }

    // END OF THE INTEGRATION PERIOD
    // If the current timestamp is greater then our update time
    // We should just "split" the NEXT IMU measurement to the update time,
    // NOTE: we add the current time, and then the time at the end of the interval (so we can get a dt)
    // NOTE: we also break out of this loop, as this is the last IMU measurement we need!
    if (imu_data.at(i + 1).timestamp > time1) {
      // If we have a very low frequency IMU then, we could have only recorded the first integration (i.e. case 1) and nothing else
      // In this case, both the current IMU measurement and the next is greater than the desired intepolation, thus we should just cut the
      // current at the desired time Else, we have hit CASE2 and this IMU measurement is not past the desired propagation time, thus add the
      // whole IMU reading
      if (imu_data.at(i).timestamp > time1 && i == 0) {
        // This case can happen if we don't have any imu data that has occured before the startup time
        // This means that either we have dropped IMU data, or we have not gotten enough.
        // In this case we can't propgate forward in time, so there is not that much we can do.
        break;
      } else if (imu_data.at(i).timestamp > time1) {
        ov_core::ImuData data = interpolate_data(imu_data.at(i - 1), imu_data.at(i), time1);
        prop_data.push_back(data);
        // PRINT_DEBUG("propagation #%d = CASE 3.1 = %.3f => %.3f\n",
        // (int)i,imu_data.at(i).timestamp-prop_data.at(0).timestamp,imu_data.at(i).timestamp-time0);
      } else {
        prop_data.push_back(imu_data.at(i));
        // PRINT_DEBUG("propagation #%d = CASE 3.2 = %.3f => %.3f\n",
        // (int)i,imu_data.at(i).timestamp-prop_data.at(0).timestamp,imu_data.at(i).timestamp-time0);
      }
      // If the added IMU message doesn't end exactly at the camera time
      // Then we need to add another one that is right at the ending time
      if (prop_data.at(prop_data.size() - 1).timestamp != time1) {
        ov_core::ImuData data = interpolate_data(imu_data.at(i), imu_data.at(i + 1), time1);
        prop_data.push_back(data);
        // PRINT_DEBUG("propagation #%d = CASE 3.3 = %.3f => %.3f\n", (int)i,data.timestamp-prop_data.at(0).timestamp,data.timestamp-time0);
      }
      break;
    }
  }

  // Check that we have at least one measurement to propagate with
  if (prop_data.empty()) {
    if (warn)
      PRINT_WARNING(
          YELLOW
          "Propagator::select_imu_readings(): No IMU measurements to propagate with (%d of 2). IMU-CAMERA are likely messed up!!!\n" RESET,
          (int)prop_data.size());
    return prop_data;
  }

  // If we did not reach the whole integration period (i.e., the last inertial measurement we have is smaller then the time we want to
  // reach) Then we should just "stretch" the last measurement to be the whole period (case 3 in the above loop)
  // if(time1-imu_data.at(imu_data.size()-1).timestamp > 1e-3) {
  //    PRINT_DEBUG(YELLOW "Propagator::select_imu_readings(): Missing inertial measurements to propagate with (%.6f sec missing).
  //    IMU-CAMERA are likely messed up!!!\n" RESET, (time1-imu_data.at(imu_data.size()-1).timestamp)); return prop_data;
  //}

  // Loop through and ensure we do not have an zero dt values
  // This would cause the noise covariance to be Infinity
  for (size_t i = 0; i < prop_data.size() - 1; i++) {
    if (std::abs(prop_data.at(i + 1).timestamp - prop_data.at(i).timestamp) < 1e-12) {
      if (warn)
        PRINT_WARNING(YELLOW "Propagator::select_imu_readings(): Zero DT between IMU reading %d and %d, removing it!\n" RESET, (int)i,
                      (int)(i + 1));
      prop_data.erase(prop_data.begin() + i);
      i--;
    }
  }

  // Check that we have at least one measurement to propagate with
  if (prop_data.size() < 2) {
    if (warn)
      PRINT_WARNING(
          YELLOW
          "Propagator::select_imu_readings(): No IMU measurements to propagate with (%d of 2). IMU-CAMERA are likely messed up!!!\n" RESET,
          (int)prop_data.size());
    return prop_data;
  }

  // Success :D
  return prop_data;
}

void Propagator::predict_and_compute(std::shared_ptr<State> state, const ov_core::ImuData &data_minus, const ov_core::ImuData &data_plus,
                                     Eigen::MatrixXd &F, Eigen::MatrixXd &Qd) {

  // Set them to zero
  F.setZero();
  Qd.setZero();

  // Time elapsed over interval
  double dt = data_plus.timestamp - data_minus.timestamp;
  // assert(data_plus.timestamp>data_minus.timestamp);

  // Corrected imu acc measurements with our current biases
  Eigen::Vector3d a_hat1 = data_minus.am - state->_imu->bias_a();
  Eigen::Vector3d a_hat2 = data_plus.am - state->_imu->bias_a();
  Eigen::Vector3d a_hat = a_hat1;
  if (state->_options.imu_avg) {
    a_hat = .5 * (a_hat1 + a_hat2);
  }

  // Convert "raw" imu to its corrected frame using the IMU intrinsics
  Eigen::Vector3d a_uncorrected = a_hat;
  a_hat = state->R_AcctoI() * state->Da() * a_hat;
  a_hat1 = state->R_AcctoI() * state->Da() * a_hat1;
  a_hat2 = state->R_AcctoI() * state->Da() * a_hat2;

  // Corrected imu gyro measurements with our current biases and gravity sensativity
  Eigen::Vector3d gravity_correction1 = state->Tg() * a_hat1;
  Eigen::Vector3d gravity_correction2 = state->Tg() * a_hat2;
  Eigen::Vector3d w_hat1 = data_minus.wm - state->_imu->bias_g() - gravity_correction1;
  Eigen::Vector3d w_hat2 = data_plus.wm - state->_imu->bias_g() - gravity_correction2;
  Eigen::Vector3d w_hat = w_hat1;
  if (state->_options.imu_avg) {
    w_hat = .5 * (w_hat1 + w_hat2);
  }

  // Convert "raw" imu to its corrected frame using the IMU intrinsics
  Eigen::Vector3d w_uncorrected = w_hat;
  w_hat = state->R_GyrotoI() * state->Dw() * w_hat;
  w_hat1 = state->R_GyrotoI() * state->Dw() * w_hat1;
  w_hat2 = state->R_GyrotoI() * state->Dw() * w_hat2;

  // Pre-compute some analytical values for the mean and covariance integration
  Eigen::Matrix<double, 3, 18> Xi_sum = Eigen::Matrix<double, 3, 18>::Zero(3, 18);
  if (state->_options.use_analytic_integration || state->_options.use_rk4_integration) {
    compute_Xi_sum(state, dt, w_hat, a_hat, Xi_sum);
  }

  // Compute the new state mean value
  Eigen::Vector4d new_q;
  Eigen::Vector3d new_v, new_p;
  if (state->_options.use_analytic_integration) {
    predict_mean_analytic(state, dt, w_hat, a_hat, new_q, new_v, new_p, Xi_sum);
  } else if (state->_options.use_rk4_integration) {
    predict_mean_rk4(state, dt, w_hat, a_hat, w_hat2, a_hat2, new_q, new_v, new_p);
  } else {
    predict_mean_discrete(state, dt, w_hat, a_hat, w_hat2, a_hat2, new_q, new_v, new_p);
  }

  // Allocate state transition and noise Jacobian
  F = Eigen::MatrixXd::Zero(state->imu_intrinsic_size() + 15, state->imu_intrinsic_size() + 15);
  Eigen::MatrixXd G = Eigen::MatrixXd::Zero(state->imu_intrinsic_size() + 15, 12);
  if (state->_options.use_analytic_integration || state->_options.use_rk4_integration) {
    compute_F_and_G_analytic(state, dt, w_hat, a_hat, w_uncorrected, a_uncorrected, new_q, new_v, new_p, Xi_sum, F, G);
  } else {
    compute_F_and_G_discrete(state, dt, w_hat, a_hat, w_uncorrected, a_uncorrected, new_q, new_v, new_p, F, G);
  }

  // Construct our discrete noise covariance matrix
  // Note that we need to convert our continuous time noises to discrete
  // Equations (129) amd (130) of Trawny tech report
  Eigen::Matrix<double, 12, 12> Qc = Eigen::Matrix<double, 12, 12>::Zero();
  Qc.block(0, 0, 3, 3) = std::pow(_imu_config.sigma_w, 2) / dt * Eigen::Matrix3d::Identity();
  Qc.block(3, 3, 3, 3) = std::pow(_imu_config.sigma_a, 2) / dt * Eigen::Matrix3d::Identity();
  Qc.block(6, 6, 3, 3) = std::pow(_imu_config.sigma_wb, 2) / dt * Eigen::Matrix3d::Identity();
  Qc.block(9, 9, 3, 3) = std::pow(_imu_config.sigma_ab, 2) / dt * Eigen::Matrix3d::Identity();

  // Compute the noise injected into the state over the interval
  Qd = Eigen::MatrixXd::Zero(state->imu_intrinsic_size() + 15, state->imu_intrinsic_size() + 15);
  Qd = G * Qc * G.transpose();
  Qd = 0.5 * (Qd + Qd.transpose());

  // Now replace imu estimate and fej with propagated values
  Eigen::Matrix<double, 16, 1> imu_x = state->_imu->value();
  imu_x.block(0, 0, 4, 1) = new_q;
  imu_x.block(4, 0, 3, 1) = new_p;
  imu_x.block(7, 0, 3, 1) = new_v;
  state->_imu->set_value(imu_x);
  state->_imu->set_fej(imu_x);
}

void Propagator::compute_F_and_G_analytic(std::shared_ptr<State> state, double dt, const Eigen::Vector3d &w_hat,
                                          const Eigen::Vector3d &a_hat, const Eigen::Vector3d &w_uncorrected,
                                          const Eigen::Vector3d &a_uncorrected, const Eigen::Vector4d &new_q, const Eigen::Vector3d &new_v,
                                          const Eigen::Vector3d &new_p, const Eigen::Matrix<double, 3, 18> &Xi_sum, Eigen::MatrixXd &F,
                                          Eigen::MatrixXd &G) {

  // Get the locations of each entry of the imu state
  int local_size = 0;
  int th_id = local_size;
  local_size += state->_imu->q()->size();
  int p_id = local_size;
  local_size += state->_imu->p()->size();
  int v_id = local_size;
  local_size += state->_imu->v()->size();
  int bg_id = local_size;
  local_size += state->_imu->bg()->size();
  int ba_id = local_size;
  local_size += state->_imu->ba()->size();

  // If we are doing calibration, we can define their "local" id in the state transition
  int Dw_id = -1;
  int Da_id = -1;
  int Tg_id = -1;
  int th_atoI_id = -1;
  int th_wtoI_id = -1;
  if (state->_options.do_calib_imu_intrinsics) {
    Dw_id = local_size;
    local_size += state->_calib_imu_dw->size();
    Da_id = local_size;
    local_size += state->_calib_imu_da->size();
    if (state->_options.do_calib_imu_g_sensitivity) {
      Tg_id = local_size;
      local_size += state->_calib_imu_tg->size();
    }
    // 0: kalibr, 1: rpng
    if (state->_options.imu_model == 0) {
      th_wtoI_id = local_size;
      local_size += state->_calib_imu_GYROtoIMU->size();
    } else {
      th_atoI_id = local_size;
      local_size += state->_calib_imu_ACCtoIMU->size();
    }
  }

  // This is the change in the orientation from the end of the last prop to the current prop
  // This is needed since we need to include the "k-th" updated orientation information
  Eigen::Matrix3d R_k = state->_imu->Rot();
  Eigen::Vector3d v_k = state->_imu->vel();
  Eigen::Vector3d p_k = state->_imu->pos();
  if (state->_options.do_fej) {
    R_k = state->_imu->Rot_fej();
    v_k = state->_imu->vel_fej();
    p_k = state->_imu->pos_fej();
  }
  Eigen::Matrix3d dR_ktok1 = quat_2_Rot(new_q) * R_k.transpose();

  Eigen::Matrix3d Da = state->Da();
  Eigen::Matrix3d Dw = state->Dw();
  Eigen::Matrix3d Tg = state->Tg();
  Eigen::Matrix3d R_atoI = state->R_AcctoI();
  Eigen::Matrix3d R_wtoI = state->R_GyrotoI();
  Eigen::Vector3d a_k = R_atoI * Da * a_uncorrected;
  Eigen::Vector3d w_k = R_wtoI * Dw * w_uncorrected; // contains gravity correction already

  Eigen::Matrix3d Xi_1 = Xi_sum.block<3, 3>(0, 3);
  Eigen::Matrix3d Xi_2 = Xi_sum.block<3, 3>(0, 6);
  Eigen::Matrix3d Jr = Xi_sum.block<3, 3>(0, 9);
  Eigen::Matrix3d Xi_3 = Xi_sum.block<3, 3>(0, 12);
  Eigen::Matrix3d Xi_4 = Xi_sum.block<3, 3>(0, 15);

  // for th
  F.block<3, 3>(th_id, th_id) = dR_ktok1;
  F.block<3, 3>(p_id, th_id).noalias() = -skew_x(new_p - p_k - v_k * dt + 0.5 * _gravity * dt * dt) * R_k.transpose();
  F.block<3, 3>(v_id, th_id).noalias() = -skew_x(new_v - v_k + _gravity * dt) * R_k.transpose();

  // for p
  F.block<3, 3>(p_id, p_id).setIdentity();

  // for v
  F.block<3, 3>(p_id, v_id) = Eigen::Matrix3d::Identity() * dt;
  F.block<3, 3>(v_id, v_id).setIdentity();

  // for bg
  F.block<3, 3>(th_id, bg_id).noalias() = -Jr * dt * R_wtoI * Dw;
  F.block<3, 3>(p_id, bg_id) = R_k.transpose() * Xi_4 * R_wtoI * Dw;
  F.block<3, 3>(v_id, bg_id) = R_k.transpose() * Xi_3 * R_wtoI * Dw;
  F.block<3, 3>(bg_id, bg_id).setIdentity();

  // for ba
  F.block<3, 3>(th_id, ba_id).noalias() = Jr * dt * R_wtoI * Dw * Tg * R_atoI * Da;
  F.block<3, 3>(p_id, ba_id) = -R_k.transpose() * (Xi_2 + Xi_4 * R_wtoI * Dw * Tg) * R_atoI * Da;
  F.block<3, 3>(v_id, ba_id) = -R_k.transpose() * (Xi_1 + Xi_3 * R_wtoI * Dw * Tg) * R_atoI * Da;
  F.block<3, 3>(ba_id, ba_id).setIdentity();

  // begin to add the state transition matrix for the omega intrinsics part
  if (Dw_id != -1) {
    F.block(th_id, Dw_id, 3, state->_calib_imu_dw->size()) = Jr * dt * R_wtoI * compute_H_Dw(state, w_uncorrected);
    F.block(p_id, Dw_id, 3, state->_calib_imu_dw->size()) = -R_k.transpose() * Xi_4 * R_wtoI * compute_H_Dw(state, w_uncorrected);
    F.block(v_id, Dw_id, 3, state->_calib_imu_dw->size()) = -R_k.transpose() * Xi_3 * R_wtoI * compute_H_Dw(state, w_uncorrected);
    F.block(Dw_id, Dw_id, state->_calib_imu_dw->size(), state->_calib_imu_dw->size()).setIdentity();
  }

  // begin to add the state transition matrix for the acc intrinsics part
  if (Da_id != -1) {
    F.block(Da_id, Da_id, state->_calib_imu_da->size(), state->_calib_imu_da->size()).setIdentity();
    F.block(th_id, Da_id, 3, state->_calib_imu_da->size()) = -Jr * dt * R_wtoI * Dw * Tg * R_atoI * compute_H_Da(state, w_uncorrected);
    F.block(p_id, Da_id, 3, state->_calib_imu_da->size()) =
        R_k.transpose() * (Xi_2 + Xi_4 * R_wtoI * Dw * Tg) * R_atoI * compute_H_Da(state, a_uncorrected);
    F.block(v_id, Da_id, 3, state->_calib_imu_da->size()) =
        R_k.transpose() * (Xi_1 + Xi_3 * R_wtoI * Dw * Tg) * R_atoI * compute_H_Da(state, a_uncorrected);
  }

  // add the state trasition matrix of the tg part
  if (Tg_id != -1) {
    F.block(Tg_id, Tg_id, state->_calib_imu_tg->size(), state->_calib_imu_tg->size()).setIdentity();
    F.block(th_id, Tg_id, 3, state->_calib_imu_tg->size()) = -Jr * dt * R_wtoI * Dw * compute_H_Tg(state, a_k);
    F.block(p_id, Tg_id, 3, state->_calib_imu_tg->size()) = R_k.transpose() * Xi_4 * R_wtoI * Dw * compute_H_Tg(state, a_k);
    F.block(v_id, Tg_id, 3, state->_calib_imu_tg->size()) = R_k.transpose() * Xi_3 * R_wtoI * Dw * compute_H_Tg(state, a_k);
  }

  // begin to add the state transition matrix for the acctoI part
  if (th_atoI_id != -1) {
    F.block<3, 3>(th_atoI_id, th_atoI_id).setIdentity();
    F.block<3, 3>(th_id, th_atoI_id) = -Jr * dt * R_wtoI * Dw * Tg * ov_core::skew_x(a_k);
    F.block<3, 3>(p_id, th_atoI_id) = R_k.transpose() * (Xi_2 + Xi_4 * R_wtoI * Dw * Tg) * ov_core::skew_x(a_k);
    F.block<3, 3>(v_id, th_atoI_id) = R_k.transpose() * (Xi_1 + Xi_3 * R_wtoI * Dw * Tg) * ov_core::skew_x(a_k);
  }

  // begin to add the state transition matrix for the gyrotoI part
  if (th_wtoI_id != -1) {
    F.block<3, 3>(th_wtoI_id, th_wtoI_id).setIdentity();
    F.block<3, 3>(th_id, th_wtoI_id) = Jr * dt * ov_core::skew_x(w_k);
    F.block<3, 3>(p_id, th_wtoI_id) = -R_k.transpose() * Xi_4 * ov_core::skew_x(w_k);
    F.block<3, 3>(v_id, th_wtoI_id) = -R_k.transpose() * Xi_3 * ov_core::skew_x(w_k);
  }

  // construct the G part
  G.block<3, 3>(th_id, 0) = -Jr * dt * R_wtoI * Dw;
  G.block<3, 3>(p_id, 0) = R_k.transpose() * Xi_4 * R_wtoI * Dw;
  G.block<3, 3>(v_id, 0) = R_k.transpose() * Xi_3 * R_wtoI * Dw;
  G.block<3, 3>(th_id, 3) = Jr * dt * R_wtoI * Dw * Tg * R_atoI * Da;
  G.block<3, 3>(p_id, 3) = -R_k.transpose() * (Xi_2 + Xi_4 * R_wtoI * Dw * Tg) * R_atoI * Da;
  G.block<3, 3>(v_id, 3) = -R_k.transpose() * (Xi_1 + Xi_3 * R_wtoI * Dw * Tg) * R_atoI * Da;
  G.block<3, 3>(bg_id, 6) = dt * Eigen::Matrix3d::Identity();
  G.block<3, 3>(ba_id, 9) = dt * Eigen::Matrix3d::Identity();
}

void Propagator::compute_F_and_G_discrete(std::shared_ptr<State> state, double dt, const Eigen::Vector3d &w_hat,
                                          const Eigen::Vector3d &a_hat, const Eigen::Vector3d &w_uncorrected,
                                          const Eigen::Vector3d &a_uncorrected, const Eigen::Vector4d &new_q, const Eigen::Vector3d &new_v,
                                          const Eigen::Vector3d &new_p, Eigen::MatrixXd &F, Eigen::MatrixXd &G) {

  // Get the locations of each entry of the imu state
  int local_size = 0;
  int th_id = local_size;
  local_size += state->_imu->q()->size();
  int p_id = local_size;
  local_size += state->_imu->p()->size();
  int v_id = local_size;
  local_size += state->_imu->v()->size();
  int bg_id = local_size;
  local_size += state->_imu->bg()->size();
  int ba_id = local_size;
  local_size += state->_imu->ba()->size();

  // If we are doing calibration, we can define their "local" id in the state transition
  int Dw_id = -1;
  int Da_id = -1;
  int Tg_id = -1;
  int th_atoI_id = -1;
  int th_wtoI_id = -1;
  if (state->_options.do_calib_imu_intrinsics) {
    Dw_id = local_size;
    local_size += state->_calib_imu_dw->size();
    Da_id = local_size;
    local_size += state->_calib_imu_da->size();
    if (state->_options.do_calib_imu_g_sensitivity) {
      Tg_id = local_size;
      local_size += state->_calib_imu_tg->size();
    }
    if (state->_options.imu_model == 0) {
      // Kalibr model
      th_wtoI_id = local_size;
      local_size += state->_calib_imu_GYROtoIMU->size();
    } else {
      // RPNG model
      th_atoI_id = local_size;
      local_size += state->_calib_imu_ACCtoIMU->size();
    }
  }

  //============================================================
  //============================================================

  // This is the change in the orientation from the end of the last prop to the current prop
  // This is needed since we need to include the "k-th" updated orientation information
  Eigen::Matrix3d R_k = state->_imu->Rot();
  Eigen::Vector3d v_k = state->_imu->vel();
  Eigen::Vector3d p_k = state->_imu->pos();

  if (state->_options.do_fej) {
    R_k = state->_imu->Rot_fej();
    v_k = state->_imu->vel_fej();
    p_k = state->_imu->pos_fej();
  }
  Eigen::Matrix3d dR_ktok1 = quat_2_Rot(new_q) * R_k.transpose();

  // This is the change in the orientation from the end of the last prop to the current prop
  // This is needed since we need to include the "k-th" updated orientation information
  Eigen::Matrix3d Da = state->Da();
  Eigen::Matrix3d Dw = state->Dw();
  Eigen::Matrix3d Tg = state->Tg();
  Eigen::Matrix3d R_atoI = state->R_AcctoI();
  Eigen::Matrix3d R_wtoI = state->R_GyrotoI();
  Eigen::Vector3d a_k = R_atoI * Da * a_uncorrected;
  Eigen::Vector3d w_k = R_wtoI * Dw * w_uncorrected; // contains the gravity correction already
  Eigen::Matrix3d Jr = Jr_so3(w_k * dt);

  // for theta
  F.block<3, 3>(th_id, th_id) = dR_ktok1;
  // F.block(th_id, bg_id, 3, 3).noalias() = -Jr_so3(w_hat * dt) * dt * R_wtoI_fej * Dw_fej;
  F.block<3, 3>(th_id, bg_id).noalias() = -Jr * dt * R_wtoI * Dw;
  F.block<3, 3>(th_id, ba_id).noalias() = Jr * dt * R_wtoI * Dw * Tg * R_atoI * Da;

  // for position
  F.block<3, 3>(p_id, th_id).noalias() = -skew_x(new_p - p_k - v_k * dt + 0.5 * _gravity * dt * dt) * R_k.transpose();
  F.block<3, 3>(p_id, p_id).setIdentity();
  F.block<3, 3>(p_id, v_id) = Eigen::Matrix3d::Identity() * dt;
  F.block<3, 3>(p_id, ba_id) = -0.5 * R_k.transpose() * dt * dt * R_atoI * Da;

  // for velocity
  F.block<3, 3>(v_id, th_id).noalias() = -skew_x(new_v - v_k + _gravity * dt) * R_k.transpose();
  F.block<3, 3>(v_id, v_id).setIdentity();
  F.block<3, 3>(v_id, ba_id) = -R_k.transpose() * dt * R_atoI * Da;

  // for bg
  F.block<3, 3>(bg_id, bg_id).setIdentity();
  // for ba
  F.block<3, 3>(ba_id, ba_id).setIdentity();

  // begin to add the state transition matrix for the omega intrinsics part
  if (Dw_id != -1) {
    F.block(Dw_id, Dw_id, state->_calib_imu_dw->size(), state->_calib_imu_dw->size()).setIdentity();
    F.block(th_id, Dw_id, 3, state->_calib_imu_dw->size()) = Jr * dt * R_wtoI * compute_H_Dw(state, w_uncorrected);
  }

  // begin to add the state transition matrix for the acc intrinsics part
  if (Da_id != -1) {
    F.block(th_id, Da_id, 3, state->_calib_imu_da->size()) = -Jr * dt * R_wtoI * Tg * R_atoI * compute_H_Da(state, a_uncorrected);
    F.block(p_id, Da_id, 3, state->_calib_imu_da->size()) = 0.5 * R_k.transpose() * dt * dt * R_atoI * compute_H_Da(state, a_uncorrected);
    F.block(v_id, Da_id, 3, state->_calib_imu_da->size()) = R_k.transpose() * dt * R_atoI * compute_H_Da(state, a_uncorrected);
    F.block(Da_id, Da_id, state->_calib_imu_da->size(), state->_calib_imu_da->size()).setIdentity();
  }

  // begin to add the state transition matrix for the acc intrinsics part
  if (th_atoI_id != -1) {
    F.block<3, 3>(th_atoI_id, th_atoI_id).setIdentity();
    F.block<3, 3>(th_id, th_atoI_id) = -Jr * dt * R_wtoI * Dw * Tg * ov_core::skew_x(a_k);
    F.block<3, 3>(p_id, th_atoI_id) = 0.5 * R_k.transpose() * dt * dt * ov_core::skew_x(a_k);
    F.block<3, 3>(v_id, th_atoI_id) = R_k.transpose() * dt * ov_core::skew_x(a_k);
  }

  // begin to add the state transition matrix for the gyro intrinsics part
  if (th_wtoI_id != -1) {
    F.block<3, 3>(th_wtoI_id, th_wtoI_id).setIdentity();
    F.block<3, 3>(th_id, th_wtoI_id) = Jr * dt * ov_core::skew_x(w_k);
  }

  // begin to add the state transition matrix for the gravity sensitivity part
  if (Tg_id != -1) {
    F.block(Tg_id, Tg_id, state->_calib_imu_tg->size(), state->_calib_imu_tg->size()).setIdentity();
    F.block(th_id, Tg_id, 3, state->_calib_imu_tg->size()) = -Jr * dt * R_wtoI * Dw * compute_H_Tg(state, a_k);
  }

  // Noise jacobian
  G.block<3, 3>(th_id, 0) = -Jr * dt * R_wtoI * Dw;
  G.block<3, 3>(th_id, 3) = Jr * dt * R_wtoI * Dw * Tg * R_atoI * Da;
  G.block<3, 3>(v_id, 3) = -R_k.transpose() * dt * R_atoI * Da;
  G.block<3, 3>(p_id, 3) = -0.5 * R_k.transpose() * dt * dt * R_atoI * Da;
  G.block<3, 3>(bg_id, 6) = dt * Eigen::Matrix3d::Identity();
  G.block<3, 3>(ba_id, 9) = dt * Eigen::Matrix3d::Identity();
}

void Propagator::predict_mean_discrete(std::shared_ptr<State> state, double dt, const Eigen::Vector3d &w_hat1,
                                       const Eigen::Vector3d &a_hat1, const Eigen::Vector3d &w_hat2, const Eigen::Vector3d &a_hat2,
                                       Eigen::Vector4d &new_q, Eigen::Vector3d &new_v, Eigen::Vector3d &new_p) {

  // If we are averaging the IMU, then do so
  Eigen::Vector3d w_hat = w_hat1;
  Eigen::Vector3d a_hat = a_hat1;
  if (state->_options.imu_avg) {
    w_hat = .5 * (w_hat1 + w_hat2);
    a_hat = .5 * (a_hat1 + a_hat2);
  }

  // Pre-compute things
  double w_norm = w_hat.norm();
  Eigen::Matrix<double, 4, 4> I_4x4 = Eigen::Matrix<double, 4, 4>::Identity();
  Eigen::Matrix3d R_Gtoi = state->_imu->Rot();

  // Orientation: Equation (101) and (103) and of Trawny indirect TR
  Eigen::Matrix<double, 4, 4> bigO;
  if (w_norm > 1e-12) {
    bigO = cos(0.5 * w_norm * dt) * I_4x4 + 1 / w_norm * sin(0.5 * w_norm * dt) * Omega(w_hat);
  } else {
    bigO = I_4x4 + 0.5 * dt * Omega(w_hat);
  }
  new_q = quatnorm(bigO * state->_imu->quat());
  // new_q = rot_2_quat(exp_so3(-w_hat*dt)*R_Gtoi);

  // Velocity: just the acceleration in the local frame, minus global gravity
  new_v = state->_imu->vel() + R_Gtoi.transpose() * a_hat * dt - _gravity * dt;

  // Position: just velocity times dt, with the acceleration integrated twice
  new_p = state->_imu->pos() + state->_imu->vel() * dt + 0.5 * R_Gtoi.transpose() * a_hat * dt * dt - 0.5 * _gravity * dt * dt;
}

void Propagator::predict_mean_analytic(std::shared_ptr<State> state, double dt, const Eigen::Vector3d &w_hat, const Eigen::Vector3d &a_hat,
                                       Eigen::Vector4d &new_q, Eigen::Vector3d &new_v, Eigen::Vector3d &new_p,
                                       Eigen::Matrix<double, 3, 18> &Xi_sum) {

  // Pre-compute things
  Eigen::Matrix3d R_Gtok = state->_imu->Rot();

  // get the pre-computed value
  Eigen::Matrix3d R_k1_to_k = Xi_sum.block<3, 3>(0, 0);
  Eigen::Matrix3d Xi_1 = Xi_sum.block<3, 3>(0, 3);
  Eigen::Matrix3d Xi_2 = Xi_sum.block<3, 3>(0, 6);

  // use the precomputed value to get the new state
  Eigen::Matrix<double, 4, 1> q_k_to_k1 = ov_core::rot_2_quat(R_k1_to_k.transpose());
  new_q = ov_core::quat_multiply(q_k_to_k1, state->_imu->quat());

  // Velocity: just the acceleration in the local frame, minus global gravity
  new_v = state->_imu->vel() + R_Gtok.transpose() * Xi_1 * a_hat - _gravity * dt;

  // Position: just velocity times dt, with the acceleration integrated twice
  new_p = state->_imu->pos() + state->_imu->vel() * dt + R_Gtok.transpose() * Xi_2 * a_hat - 0.5 * _gravity * dt * dt;
}

void Propagator::predict_mean_rk4(std::shared_ptr<State> state, double dt, const Eigen::Vector3d &w_hat1, const Eigen::Vector3d &a_hat1,
                                  const Eigen::Vector3d &w_hat2, const Eigen::Vector3d &a_hat2, Eigen::Vector4d &new_q,
                                  Eigen::Vector3d &new_v, Eigen::Vector3d &new_p) {

  // Pre-compute things
  Eigen::Vector3d w_hat = w_hat1;
  Eigen::Vector3d a_hat = a_hat1;
  Eigen::Vector3d w_alpha = (w_hat2 - w_hat1) / dt;
  Eigen::Vector3d a_jerk = (a_hat2 - a_hat1) / dt;

  // y0 ================
  Eigen::Vector4d q_0 = state->_imu->quat();
  Eigen::Vector3d p_0 = state->_imu->pos();
  Eigen::Vector3d v_0 = state->_imu->vel();

  // k1 ================
  Eigen::Vector4d dq_0 = {0, 0, 0, 1};
  Eigen::Vector4d q0_dot = 0.5 * Omega(w_hat) * dq_0;
  Eigen::Vector3d p0_dot = v_0;
  Eigen::Matrix3d R_Gto0 = quat_2_Rot(quat_multiply(dq_0, q_0));
  Eigen::Vector3d v0_dot = R_Gto0.transpose() * a_hat - _gravity;

  Eigen::Vector4d k1_q = q0_dot * dt;
  Eigen::Vector3d k1_p = p0_dot * dt;
  Eigen::Vector3d k1_v = v0_dot * dt;

  // k2 ================
  w_hat += 0.5 * w_alpha * dt;
  a_hat += 0.5 * a_jerk * dt;

  Eigen::Vector4d dq_1 = quatnorm(dq_0 + 0.5 * k1_q);
  // Eigen::Vector3d p_1 = p_0+0.5*k1_p;
  Eigen::Vector3d v_1 = v_0 + 0.5 * k1_v;

  Eigen::Vector4d q1_dot = 0.5 * Omega(w_hat) * dq_1;
  Eigen::Vector3d p1_dot = v_1;
  Eigen::Matrix3d R_Gto1 = quat_2_Rot(quat_multiply(dq_1, q_0));
  Eigen::Vector3d v1_dot = R_Gto1.transpose() * a_hat - _gravity;

  Eigen::Vector4d k2_q = q1_dot * dt;
  Eigen::Vector3d k2_p = p1_dot * dt;
  Eigen::Vector3d k2_v = v1_dot * dt;

  // k3 ================
  Eigen::Vector4d dq_2 = quatnorm(dq_0 + 0.5 * k2_q);
  // Eigen::Vector3d p_2 = p_0+0.5*k2_p;
  Eigen::Vector3d v_2 = v_0 + 0.5 * k2_v;

  Eigen::Vector4d q2_dot = 0.5 * Omega(w_hat) * dq_2;
  Eigen::Vector3d p2_dot = v_2;
  Eigen::Matrix3d R_Gto2 = quat_2_Rot(quat_multiply(dq_2, q_0));
  Eigen::Vector3d v2_dot = R_Gto2.transpose() * a_hat - _gravity;

  Eigen::Vector4d k3_q = q2_dot * dt;
  Eigen::Vector3d k3_p = p2_dot * dt;
  Eigen::Vector3d k3_v = v2_dot * dt;

  // k4 ================
  w_hat += 0.5 * w_alpha * dt;
  a_hat += 0.5 * a_jerk * dt;

  Eigen::Vector4d dq_3 = quatnorm(dq_0 + k3_q);
  // Eigen::Vector3d p_3 = p_0+k3_p;
  Eigen::Vector3d v_3 = v_0 + k3_v;

  Eigen::Vector4d q3_dot = 0.5 * Omega(w_hat) * dq_3;
  Eigen::Vector3d p3_dot = v_3;
  Eigen::Matrix3d R_Gto3 = quat_2_Rot(quat_multiply(dq_3, q_0));
  Eigen::Vector3d v3_dot = R_Gto3.transpose() * a_hat - _gravity;

  Eigen::Vector4d k4_q = q3_dot * dt;
  Eigen::Vector3d k4_p = p3_dot * dt;
  Eigen::Vector3d k4_v = v3_dot * dt;

  // y+dt ================
  Eigen::Vector4d dq = quatnorm(dq_0 + (1.0 / 6.0) * k1_q + (1.0 / 3.0) * k2_q + (1.0 / 3.0) * k3_q + (1.0 / 6.0) * k4_q);
  new_q = quat_multiply(dq, q_0);
  new_p = p_0 + (1.0 / 6.0) * k1_p + (1.0 / 3.0) * k2_p + (1.0 / 3.0) * k3_p + (1.0 / 6.0) * k4_p;
  new_v = v_0 + (1.0 / 6.0) * k1_v + (1.0 / 3.0) * k2_v + (1.0 / 3.0) * k3_v + (1.0 / 6.0) * k4_v;
}

void Propagator::compute_Xi_sum(std::shared_ptr<State> state, double d_t, const Eigen::Vector3d &w_hat, const Eigen::Vector3d &a_hat,
                                Eigen::Matrix<double, 3, 18> &Xi_sum) {

  // useful identities
  Eigen::Matrix3d I_3x3 = Eigen::Matrix3d::Identity();
  Eigen::Vector3d Z_3x1 = Eigen::MatrixXd::Zero(3, 1);

  // now begin the integration
  double w_norm = w_hat.norm();
  double d_th = w_norm * d_t;
  Eigen::Vector3d k_hat;
  if (w_norm < 1e-8) {
    k_hat << Z_3x1;
  } else {
    k_hat << w_hat / w_norm;
  }

  // comupute usefull identities
  double d_t2 = std::pow(d_t, 2);
  double d_t3 = std::pow(d_t, 3);
  ;
  double w_norm2 = std::pow(w_norm, 2);
  double w_norm3 = std::pow(w_norm, 3);
  double cos_dth = cos(d_th);
  double sin_dth = sin(d_th);
  double d_th2 = std::pow(d_th, 2);
  double d_th3 = std::pow(d_th, 3);
  Eigen::Matrix3d sK = ov_core::skew_x(k_hat);
  Eigen::Matrix3d sK2 = sK * sK;
  Eigen::Matrix3d sA = ov_core::skew_x(a_hat);

  // based on the delta theta, let's decide which integration will be used
  bool small_w = (w_norm < 1.0 / 180 * M_PI / 2);
  bool small_th = (d_th < 1.0 / 180 * M_PI / 2);

  // integration components will be used later
  Eigen::Matrix3d R_k1tok, Xi_1, Xi_2, Jr_k1tok, Xi_3, Xi_4;

  // now the R and J can be computed
  R_k1tok = I_3x3 + sin_dth * sK + (1.0 - cos_dth) * sK2;
  if (!small_th) {
    Jr_k1tok = I_3x3 - (1.0 - cos_dth) / d_th * sK + (d_th - sin_dth) / d_th * sK2;
  } else {
    Jr_k1tok = I_3x3 - sin_dth * sK + (1.0 - cos_dth) * sK2;
  }

  // now begin the integration
  if (!small_w) {

    // first order rotation integration with constant omega
    Xi_1 = I_3x3 * d_t + (1.0 - cos_dth) / w_norm * sK + (d_t - sin_dth / w_norm) * sK2;

    // second order rotation integration with constat omega
    Xi_2 = 1.0 / 2 * d_t2 * I_3x3 + (d_th - sin_dth) / w_norm2 * sK + (1.0 / 2 * d_t2 - (1.0 - cos_dth) / w_norm2) * sK2;

    // first order RAJ integratioin with constant omega and constant acc
    Xi_3 = 1.0 / 2 * d_t2 * sA + (sin_dth - d_th) / w_norm2 * sA * sK + (sin_dth - d_th * cos_dth) / w_norm2 * sK * sA +
           (1.0 / 2 * d_t2 - (1.0 - cos_dth) / w_norm2) * sA * sK2 +
           (1.0 / 2 * d_t2 - (1.0 - cos_dth - d_th * sin_dth) / w_norm2) * (sK2 * sA + k_hat.dot(a_hat) * sK) -
           (3 * sin_dth - 2 * d_th - d_th * cos_dth) / w_norm2 * k_hat.dot(a_hat) * sK2;

    // second order RAJ integration with constant omega and constant acc
    Xi_4 = 1.0 / 6 * d_t3 * sA + (2 * (1.0 - cos_dth) - d_th2) / (2 * w_norm3) * sA * sK +
           ((2 * (1.0 - cos_dth) - d_th * sin_dth) / w_norm3) * sK * sA + ((sin_dth - d_th) / w_norm3 + d_t3 / 6) * sA * sK2 +
           ((d_th - 2 * sin_dth + 1.0 / 6 * d_th3 + d_th * cos_dth) / w_norm3) * (sK2 * sA + k_hat.dot(a_hat) * sK) +
           (4 * cos_dth - 4 + d_th2 + d_th * sin_dth) / w_norm3 * k_hat.dot(a_hat) * sK2;

  } else {

    // first order rotation
    Xi_1 = d_t * (I_3x3 + sin_dth * sK + (1.0 - cos_dth) * sK2);

    // second order rotation
    Xi_2 = 1.0 / 2 * d_t * Xi_1;
    // iint_R = 1.0/2 * d_t2 * (I_3x3 + sin_dth * sK + (1.0-cos_dth) * sK2);

    // first order RAJ
    Xi_3 = 1.0 / 2 * d_t2 *
           (sA + sin_dth * (-sA * sK + sK * sA + k_hat.dot(a_hat) * sK2) + (1.0 - cos_dth) * (sA * sK2 + sK2 * sA + k_hat.dot(a_hat) * sK));

    // second order RAJ
    Xi_4 = 1.0 / 3 * d_t * Xi_3;
  }

  // store the integrated parameters
  Xi_sum << R_k1tok, Xi_1, Xi_2, Jr_k1tok, Xi_3, Xi_4;

  // we are good to go
  return;
}

Eigen::MatrixXd Propagator::compute_H_Dw(std::shared_ptr<State> state, const Eigen::Vector3d &w_uncorrected) {

  Eigen::Matrix3d I_3x3 = Eigen::MatrixXd::Identity(3, 3);
  Eigen::Vector3d e_1 = I_3x3.block<3, 1>(0, 0);
  Eigen::Vector3d e_2 = I_3x3.block<3, 1>(0, 1);
  Eigen::Vector3d e_3 = I_3x3.block<3, 1>(0, 2);

  double w_1 = w_uncorrected(0);
  double w_2 = w_uncorrected(1);
  double w_3 = w_uncorrected(2);

  // intrinsic parameters
  Eigen::MatrixXd H_Dw;
  if (state->_options.do_calib_imu_intrinsics) {
    H_Dw = Eigen::MatrixXd::Zero(3, 6);
    if (state->_options.imu_model == 0) {
      // Kalibr model
      H_Dw << w_1 * I_3x3, w_2 * e_2, w_2 * e_3, w_3 * e_3;
    } else {
      // RPNG model
      H_Dw << w_1 * e_1, w_2 * e_1, w_2 * e_2, w_3 * I_3x3;
    }
  }

  // we are good to go
  return H_Dw;
}

Eigen::MatrixXd Propagator::compute_H_Da(std::shared_ptr<State> state, const Eigen::Vector3d &a_uncorrected) {

  Eigen::Matrix3d I_3x3 = Eigen::MatrixXd::Identity(3, 3);
  Eigen::Vector3d e_1 = I_3x3.block<3, 1>(0, 0);
  Eigen::Vector3d e_2 = I_3x3.block<3, 1>(0, 1);
  Eigen::Vector3d e_3 = I_3x3.block<3, 1>(0, 2);

  double a_1 = a_uncorrected(0);
  double a_2 = a_uncorrected(1);
  double a_3 = a_uncorrected(2);

  // intrinsic parameters
  Eigen::MatrixXd H_Da;
  if (state->_options.do_calib_imu_intrinsics) {
    H_Da = Eigen::MatrixXd::Zero(3, 6);
    if (state->_options.imu_model == 0) {
      // kalibr model
      H_Da << a_1 * I_3x3, a_2 * e_2, a_2 * e_3, a_3 * e_3;
    } else {
      // RPNG model
      H_Da << a_1 * e_1, a_2 * e_1, a_2 * e_2, a_3 * I_3x3;
    }
  }

  // we are good to go
  return H_Da;
}

Eigen::MatrixXd Propagator::compute_H_Tg(std::shared_ptr<State> state, const Eigen::Vector3d &a_inI) {

  Eigen::Matrix3d I_3x3 = Eigen::MatrixXd::Identity(3, 3);
  double a_1 = a_inI(0);
  double a_2 = a_inI(1);
  double a_3 = a_inI(2);

  // intrinsic parameters
  Eigen::MatrixXd H_Tg = Eigen::MatrixXd::Zero(3, 9);
  if (state->_options.do_calib_imu_intrinsics && state->_options.do_calib_imu_g_sensitivity) {
    H_Tg << a_1 * I_3x3, a_2 * I_3x3, a_3 * I_3x3;
  }

  // we are good to go
  return H_Tg;
}