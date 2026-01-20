#pragma once

#include <cmath>
#include <iostream>
#include <vector>


namespace http_server {

class KalmanFilter {
public:
  // State: [position, velocity]
  double x_ = 0.0; // Estimated Load
  double v_ = 0.0; // Estimated Velocity

  // Covariance Matrix P (2x2)
  // [ p00 p01 ]
  // [ p10 p11 ]
  double p00_ = 1.0, p01_ = 0.0;
  double p10_ = 0.0, p11_ = 1.0;

  // Process Noise Q (TUNABLE)
  // Uncertainty in the model itself (e.g. erratic load shifts)
  double q_pos_ = 0.1;
  double q_vel_ = 0.1;

  // Measurement Noise R (TUNABLE)
  // Uncertainty in the measurement (jitter)
  double r_ = 5.0;

public:
  KalmanFilter() = default;

  // Initialize state if needed
  void init(double initial_load) {
    x_ = initial_load;
    v_ = 0.0;
    // High initial uncertainty?
    p00_ = 100.0;
    p11_ = 100.0;
  }

  // Step A: Prediction
  void predict(double dt) {
    // X = F * X
    // x = x + v * dt
    // v = v
    x_ += v_ * dt;

    // P = F * P * F^T + Q
    // F = [[1, dt], [0, 1]]
    // This arithmetic can be expanded manually for 2x2

    // P_new = F * P
    double np00 = p00_ + p10_ * dt;
    double np01 = p01_ + p11_ * dt;
    double np10 = p10_;
    double np11 = p11_;

    // P_next = P_new * F^T + Q
    // F^T = [[1, 0], [dt, 1]]

    double final_p00 =
        np00 * 1.0 + np01 * dt + q_pos_ * dt; // rough approx for Q scaling?
    // Standard Q for const velocity usually involves dt^4/4 etc but let's keep
    // it simple diagonal Q
    final_p00 = np00 + np01 * dt + (q_pos_ * dt);

    double final_p01 = np00 * 0.0 + np01 * 1.0;
    double final_p10 = np10 * 1.0 + np11 * dt;
    double final_p11 = np10 * 0.0 + np11 * 1.0 + (q_vel_ * dt);

    p00_ = final_p00;
    p01_ = final_p01;
    p10_ = final_p10;
    p11_ = final_p11;
  }

  // Step B: Update
  void update(double measurement) {
    // y = z - H * x (Innovation)
    // H = [1, 0] so H*x = x_
    double y = measurement - x_;

    // S = H * P * H^T + R (Innovation Covariance)
    // H * P = [p00, p01]
    // [p00, p01] * [1, 0]^T = p00
    double s = p00_ + r_;

    // K = P * H^T * S^-1 (Kalman Gain)
    // K = [p00, p01]^T / S = [p00/S, p10/S] (Note P is symmetric usually so
    // p10=p01)
    double k0 = p00_ / s;
    double k1 = p10_ / s;

    // X = X + K * y
    x_ += k0 * y;
    v_ += k1 * y;

    // P = (I - K * H) * P
    // I - KH = [[1,0],[0,1]] - [[k0, 0], [k1, 0]]
    //        = [[1-k0, 0], [-k1, 1]]

    double new_p00 = (1.0 - k0) * p00_;
    double new_p01 = (1.0 - k0) * p01_;
    double new_p10 = -k1 * p00_ + p10_;
    double new_p11 = -k1 * p01_ + p11_;

    p00_ = new_p00;
    p01_ = new_p01;
    p10_ = new_p10;
    p11_ = new_p11;
  }

  // Project state forward time_horizon seconds
  double predict_future_load(double time_horizon) const {
    double pred = x_ + v_ * time_horizon;
    if (pred < 0)
      return 0.0;
    return pred;
  }
};

} // namespace http_server
