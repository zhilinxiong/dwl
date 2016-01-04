#include <dwl/simulation/PreviewLocomotion.h>


namespace dwl
{

namespace simulation
{

PreviewLocomotion::PreviewLocomotion() : sample_time_(0.001), gravity_(9.81), mass_(0.),
		step_height_(0.1), force_threshold_(0.)
{
	actual_system_com_.setZero();
}


PreviewLocomotion::~PreviewLocomotion()
{

}


void PreviewLocomotion::resetFromURDFFile(std::string filename)
{
	// Reading the file
	std::ifstream model_file(filename.c_str());
	if (!model_file) {
		std::cerr << "Error opening file '" << filename << "'." << std::endl;
		abort();
	}

	// Reserving memory for the contents of the file
	std::string model_xml_string;
	model_file.seekg(0, std::ios::end);
	model_xml_string.reserve(model_file.tellg());
	model_file.seekg(0, std::ios::beg);
	model_xml_string.assign((std::istreambuf_iterator<char>(model_file)),
			std::istreambuf_iterator<char>());
	model_file.close();

	resetFromURDFFile(model_xml_string);
}


void PreviewLocomotion::resetFromURDFModel(std::string urdf_model)
{
	// Resetting the model of the floating-base system
	system_.resetFromURDFModel(urdf_model);

	// Initializing the dynamics from the URDF model
	dynamics_.modelFromURDFModel(urdf_model);

	// Setting the gravity magnitude from the rigid-body dynamic model
	gravity_ = system_.getRBDModel().gravity.norm();

	// Getting the total mass of the system
	mass_ = system_.getTotalMass();

	// Getting the floating-base CoM
	actual_system_com_ = system_.getFloatingBaseCoM();
}


void PreviewLocomotion::setSampleTime(double sample_time)
{
	sample_time_ = sample_time;
}


void PreviewLocomotion::setModel(const SLIPModel& model)
{
	slip_ = model;
}


void PreviewLocomotion::setStepHeight(double step_height)
{
	step_height_ = step_height;
}


void PreviewLocomotion::setForceThreshold(double force_threshold)
{
	force_threshold_ = force_threshold;
}


void PreviewLocomotion::multiPhasePreview(PreviewTrajectory& trajectory,
										  const PreviewState& state,
										  const MultiPhasePreviewControl& control)
{
	// Getting the number of phases
	unsigned int num_phases = control.size();

	// Computing the preview for multi-phase
	for (unsigned int i = 0; i < num_phases; i++) {
		PreviewTrajectory phase_traj;

		PreviewControl actual_control = control[i];
		// Getting the actual preview state for this phase
		PreviewState actual_state;
		if (i == 0)
			actual_state = state;
		else
			actual_state = trajectory.back();

		// Computing the preview of the actual phase
		stancePreview(phase_traj, actual_state, actual_control);
		addSwingPattern(phase_traj, actual_state, actual_control);

		// Appending the actual phase trajectory
		trajectory.insert(trajectory.end(), phase_traj.begin(), phase_traj.end());
	}
}


void PreviewLocomotion::stancePreview(PreviewTrajectory& trajectory,
									  const PreviewState& state,
									  const PreviewControl& control)
{
	// Computing the coefficients of the Spring Loaded Inverted Pendulum (SLIP) response
	double slip_omega = sqrt(gravity_ / slip_.height);
	double alpha = 2 * slip_omega * control.duration;
	Eigen::Vector2d slip_hor_proj = (state.com_pos - state.cop).head<2>();
	Eigen::Vector2d cop_disp = state.cop.head<2>() - control.terminal_cop;
	Eigen::Vector2d slip_hor_disp = state.com_vel.head<2>() * control.duration;
	Eigen::Vector2d beta_1 = slip_hor_proj / 2 + (slip_hor_disp - cop_disp) / alpha;
	Eigen::Vector2d beta_2 = slip_hor_proj / 2 - (slip_hor_disp - cop_disp) / alpha;

	// Computing the initial length of the pendulum
	double initial_length = (state.com_pos - state.cop).norm();

	// Computing the coefficients of the spring-mass system response
	double spring_omega = sqrt(slip_.stiffness / mass_);
	double delta_length = control.terminal_length - initial_length;
	double d_1 = state.com_pos(rbd::Z) - initial_length + gravity_ /
			pow(spring_omega,2);
	double d_2 = state.com_vel(rbd::Z) / spring_omega -
			delta_length / (spring_omega * control.duration);

	// Computing the preview trajectory
	unsigned int num_samples = round(control.duration / sample_time_);
	trajectory.resize(num_samples);
	for (unsigned int k = 0; k < num_samples; k++) {
		double time = sample_time_ * (k + 1);

		// Computing the current time of the preview trajectory
		PreviewState current_state;
		current_state.time = state.time + time;

		// Computing the horizontal motion of the CoM according to the SLIP system
		current_state.com_pos.head<2>() = beta_1 * exp(slip_omega * time) +
				beta_2 * exp(-slip_omega * time) +
				(cop_disp / control.duration) * time + state.cop.head<2>();
		current_state.com_vel.head<2>() = beta_1 * slip_omega * exp(slip_omega * time) -
				beta_2 * slip_omega * exp(-slip_omega * time) +
				cop_disp / control.duration;
		current_state.com_vel.head<2>() = beta_1 * pow(slip_omega,2) * exp(slip_omega * time) +
				beta_2 * pow(slip_omega,2) * exp(-slip_omega * time);

		// Computing the vertical motion of the CoM according to the spring-mass system
		current_state.com_pos(rbd::Z) = d_1 * cos(spring_omega * time) +
				d_2 * sin(spring_omega * time) + (delta_length / control.duration) * time +
				initial_length - gravity_ / pow(spring_omega,2);
		current_state.com_vel(rbd::Z) = -d_1 * spring_omega * sin(spring_omega * time) +
				d_2 * spring_omega * cos(spring_omega * time) +
				delta_length / control.duration;
		current_state.com_acc(rbd::Z) = -d_1 * pow(spring_omega,2) * cos(spring_omega * time) -
				d_2 * pow(spring_omega,2) * sin(spring_omega * time);

		// Computing the heading motion according to heading kinematic equation
		current_state.head_pos = state.head_pos + state.head_vel * time +
				0.5 * control.head_acc * pow(time,2);
		current_state.head_vel = state.head_vel + control.head_acc * time;
		current_state.head_acc = control.head_acc;

		// Appending the current state to the preview trajectory
		trajectory[k] = current_state;
	}
}


void PreviewLocomotion::flightPreview(PreviewTrajectory& trajectory,
						   	   	   	  const PreviewState& state,
									  const PreviewControl& control)
{
	// Setting the gravity vector
	Eigen::Vector3d gravity_vec = Eigen::Vector3d::Zero();
	gravity_vec(rbd::Z) = -gravity_;

	// Computing the preview trajectory
	unsigned int num_samples = round(control.duration / sample_time_);
	for (unsigned int k = 0; k < num_samples; k++) {
		double time = sample_time_ * (k + 1);

		// Computing the current time of the preview trajectory
		PreviewState current_state;
		current_state.time = state.time + time;

		// Computing the CoM motion according to the projectile EoM
		current_state.com_pos = state.com_pos + state.com_vel * time +
				0.5 * gravity_vec * pow(time,2);
		current_state.com_vel = state.com_vel + gravity_vec * time;
		current_state.com_acc = gravity_vec;

		// Computing the heading motion by assuming that there isn't change in the angular momentum
		current_state.head_pos = state.head_pos + state.head_vel * time;
		current_state.head_vel = state.head_vel;
		current_state.head_acc = 0.;
	}
}


void PreviewLocomotion::addSwingPattern(PreviewTrajectory& trajectory,
										const PreviewState& state,
										const PreviewControl& control)
{
	// Getting the actual time and sample time
	double sample_time = trajectory[1].time - trajectory[0].time;

	for (rbd::BodyVector::const_iterator contact_it = state.foot_pos.begin();
			contact_it != state.foot_pos.end(); contact_it++) {
		std::string name = contact_it->first;

		// Getting the actual position of the contact
		Eigen::Vector3d actual_pos = (Eigen::Vector3d) state.foot_pos.find(name)->second;

		Eigen::Vector3d target_pos;
		rbd::BodyVector::const_iterator swing_it = control.foot_target.find(name);
		if (swing_it != control.foot_target.end()) {
			// Getting the target position of the contact
			target_pos = (Eigen::Vector3d) swing_it->second;

			// Initializing the foot pattern generator
			simulation::StepParameters step_params(control.duration, step_height_);
			foot_pattern_generator_.setParameters(state.time,
												  actual_pos,
												  target_pos,
												  step_params);

			// Computing the swing trajectory
			Eigen::Vector3d foot_pos, foot_vel, foot_acc;
			unsigned int num_samples = round(control.duration / sample_time);
			for (unsigned int k = 0; k < num_samples; k++) {
				double time = state.time + sample_time * (k + 1);
				if (time > state.time + control.duration)
					time = state.time + control.duration;

				// Generating the swing positions, velocities and accelerations
				foot_pattern_generator_.generateTrajectory(foot_pos,
														   foot_vel,
														   foot_acc,
														   time);

				// Adding the swing state to the trajectory
				trajectory[k].foot_pos[name] = foot_pos;
				trajectory[k].foot_vel[name] = foot_vel;
				trajectory[k].foot_acc[name] = foot_acc;
			}
		} else {
			// There is not swing trajectory to generated (foot on ground). Nevertheless, we have
			// to updated their positions w.r.t the base frame
			Eigen::Vector3d foot_pos, foot_vel, foot_acc;
			unsigned int num_samples = round(control.duration / sample_time);
			Eigen::Vector3d actual_base_pos = trajectory[0].com_pos - actual_system_com_;
			for (unsigned int k = 0; k < num_samples; k++) {
				double time = state.time + sample_time * (k + 1);
				if (time > state.time + control.duration)
					time = state.time + control.duration;

				Eigen::Vector3d base_pos = trajectory[k].com_pos - actual_system_com_;


				// Adding the swing state to the trajectory
				trajectory[k].foot_pos[name] = (actual_pos + actual_base_pos) - base_pos;
				trajectory[k].foot_vel[name] = Eigen::Vector3d::Zero();
				trajectory[k].foot_acc[name] = Eigen::Vector3d::Zero();
			}
		}
	}
}


void PreviewLocomotion::toWholeBodyState(WholeBodyState& full_state,
										 const PreviewState& preview_state)
{
	// Adding the time
	full_state.time = preview_state.time;

	// From the preview model we do not know the joint states, so we neglect the joint-related
	// components of the CoM
	rbd::linearPart(full_state.base_pos) = preview_state.com_pos - actual_system_com_;
	rbd::linearPart(full_state.base_vel) = preview_state.com_vel;
	rbd::linearPart(full_state.base_acc) = preview_state.com_acc;

	full_state.base_pos(rbd::AZ) = preview_state.head_pos;
	full_state.base_vel(rbd::AZ) = preview_state.head_vel;
	full_state.base_acc(rbd::AZ) = preview_state.head_acc;

	// Contact positions
	full_state.contact_pos = preview_state.foot_pos;
	full_state.contact_vel = preview_state.foot_vel;
	full_state.contact_acc = preview_state.foot_acc;
}


void PreviewLocomotion::fromWholeBodyState(PreviewState& preview_state,
										   const WholeBodyState& full_state)
{
	// Adding the actual time
	preview_state.time = full_state.time;

	// Computing the CoM position, velocity and acceleration
	actual_system_com_ = system_.getSystemCoM(rbd::Vector6d::Zero(), full_state.joint_pos);
	preview_state.com_pos = system_.getSystemCoM(full_state.base_pos, full_state.joint_pos);
	preview_state.com_vel = system_.getSystemCoMRate(full_state.base_pos, full_state.joint_pos,
													 full_state.base_vel, full_state.joint_vel);
	preview_state.com_acc = full_state.base_acc.segment<3>(rbd::LX); //Neglecting the joint accelerations components
	preview_state.head_pos = full_state.base_pos(rbd::AZ);
	preview_state.head_vel = full_state.base_vel(rbd::AZ);
	preview_state.head_acc = full_state.base_acc(rbd::AZ);

	// Getting the world to base transformation
	Eigen::Vector3d base_traslation = full_state.base_pos.segment<3>(rbd::LX);
	Eigen::Vector3d base_rpy = full_state.base_pos.segment<3>(rbd::AX);
	Eigen::Matrix3d base_rotation = math::getRotationMatrix(base_rpy);

	// Computing the CoP in the world frame
	Eigen::Vector3d cop_wrt_base;
	dynamics_.computeCenterOfPressure(cop_wrt_base,
									  full_state.contact_eff,
									  full_state.contact_pos,
									  system_.getEndEffectorNames());
	preview_state.cop = base_traslation + base_rotation * cop_wrt_base;

	// Getting the support region by detecting the active contacts
	rbd::BodySelector active_contacts;
	dynamics_.getActiveContacts(active_contacts,
								full_state.contact_eff,
								force_threshold_);

	unsigned int num_active_contacts = active_contacts.size();
	for (unsigned int i = 0; i < num_active_contacts; i++) {
		std::string name = active_contacts[i];

		preview_state.support_region[name] = full_state.contact_pos.find(name)->second;
	}

	// Adding the contact positions, velocities and accelerations w.r.t the base frame
	preview_state.foot_pos = full_state.contact_pos;
	preview_state.foot_vel = full_state.contact_vel;
	preview_state.foot_acc = full_state.contact_acc;
}


void PreviewLocomotion::toWholeBodyTrajectory(WholeBodyTrajectory& full_traj,
											  const PreviewTrajectory& preview_traj)
{
	// Getting the number of points defined in the preview trajectory
	unsigned int traj_size = preview_traj.size();

	// Resizing the full trajectory vector
	full_traj.resize(traj_size);

	// Getting the full trajectory
	dwl::WholeBodyState full_state;
	for (unsigned int k = 0; k < traj_size; k++) {
		toWholeBodyState(full_state, preview_traj[k]);

		full_traj[k] = full_state;
	}
}

} //@namespace simulation
} //@namespace dwl