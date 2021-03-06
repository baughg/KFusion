/*

Copyright (c) 2014 University of Edinburgh, Imperial College, University of
Manchester.
Developed in the PAMELA project, EPSRC Programme Grant EP/K008730/1

This code is licensed under the MIT License.

*/
#include "kf_helper.h"

#include <kernels.h>
#include <interface.h>
#include <stdint.h>
#include <vector>
#include <sstream>
#include <string>
#include <cstring>
#include <time.h>
#include <csignal>

#include <sys/types.h>
#include <sys/stat.h>
#include <sstream>
#include <iomanip>
#include <getopt.h>

inline double tock() {
	synchroniseDevices();
#ifdef __APPLE__
	clock_serv_t cclock;
	mach_timespec_t clockData;
	host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
	clock_get_time(cclock, &clockData);
	mach_port_deallocate(mach_task_self(), cclock);
#else
	struct timespec clockData;
	clockData.tv_nsec = 0;
	clockData.tv_sec = time(0);
	// clock_gettime(CLOCK_MONOTONIC, &clockData);
#endif
	return (double)clockData.tv_sec + clockData.tv_nsec / 1000000000.0;
}

/***
* This program loop over a scene recording
*/

int main(int argc, char **argv) {

	Configuration config(argc, argv);

	// ========= CHECK ARGS =====================

	std::ostream *logstream = &std::cout;
	std::ofstream logfilestream;
	assert(config.compute_size_ratio > 0);
	assert(config.integration_rate > 0);
	assert(config.volume_size.x > 0);
	assert(config.volume_resolution.x > 0);

	if (config.log_file != "") {
		logfilestream.open(config.log_file.c_str());
		logstream = &logfilestream;
	}
	if (config.input_file == "") {
		std::cerr << "No input found." << std::endl;
		config.print_arguments();
		exit(1);
	}

	// ========= READER INITIALIZATION  =========

	DepthReader *reader;

	if (is_file(config.input_file)) {
		reader =
			new RawDepthReader(config.input_file, config.fps, config.blocking_read);

	}
	else {
		reader = new SceneDepthReader(config.input_file, config.fps,
			config.blocking_read);
	}

	std::cout.precision(10);
	std::cerr.precision(10);

	float3 init_pose = config.initial_pos_factor * config.volume_size;
	const uint2 inputSize = reader->getinputSize();
	std::cerr << "input Size is = " << inputSize.x << "," << inputSize.y
		<< std::endl;

	//  =========  BASIC PARAMETERS  (input size / computation size )  =========

	const uint2 computationSize =
		make_uint2(inputSize.x / config.compute_size_ratio,
			inputSize.y / config.compute_size_ratio);
	float4 camera = reader->getK() / config.compute_size_ratio;

	if (config.camera_overrided)
		camera = config.camera / config.compute_size_ratio;
	//  =========  BASIC BUFFERS  (input / output )  =========

	// Construction Scene reader and input buffer
	uint16_t *inputDepth =
		(uint16_t *)malloc(sizeof(uint16_t) * inputSize.x * inputSize.y);
	uchar4 *depthRender =
		(uchar4 *)malloc(sizeof(uchar4) * computationSize.x * computationSize.y);
	uchar4 *trackRender =
		(uchar4 *)malloc(sizeof(uchar4) * computationSize.x * computationSize.y);
	uchar4 *volumeRender =
		(uchar4 *)malloc(sizeof(uchar4) * computationSize.x * computationSize.y);

	uint frame = 0;

	extern float track_threshold;
	track_threshold = config.track_threshold;

	Kfusion kfusion(computationSize, config.volume_resolution, config.volume_size,
		init_pose, config.pyramid);

	double timings[7];
	timings[0] = tock();

	*logstream << "frame\tacquisition\tpreprocessing\ttracking\tintegration\trayc"
		"asting\trendering\tcomputation\ttotal    \tX          \tY     "
		"     \tZ         \ttracked   \tintegrated"
		<< std::endl;
	logstream->setf(std::ios::fixed, std::ios::floatfield);
	float* p_pose[4];

	// inter-frame
	extern Volume volume;
	extern float3 *vertex;
	extern float3 *normal;
	
	uint16_t* p_tsdf = (uint16_t*)volume.data;
	const unsigned voxel_count = volume.size.x * volume.size.y * volume.size.z;

	float* p_vertex = (float*)vertex; 
	float* p_normal = (float*)normal; 
	const unsigned pixels = computationSize.x * computationSize.y;

	config_param config_pm;
	config_pm.width = computationSize.x;
	config_pm.height = computationSize.y;
	config_pm.vol_size = volume.size.x;
	config_pm.vol_size_metric = config.volume_size.x;
	config_pm.raycast.large_step = config.mu;
	
	kfusion.get_raycast_config(
		config_pm.raycast.near_plane,
		config_pm.raycast.far_plane,
		config_pm.raycast.step,
		config_pm.raycast.large_step);

	config_pm.camera.fx = camera.x;
	config_pm.camera.fy = camera.y;
	config_pm.camera.ox = camera.z;
	config_pm.camera.oy = camera.w;

	save_config(config_pm);

	while (reader->readNextDepthFrame(inputDepth)) {

		Matrix4 pose = kfusion.getPose();
		for (int i = 0; i < 4; ++i)
			p_pose[i] = (float*)&pose.data[i];

		save_pose(frame, p_pose, true);
		float xt = pose.data[0].w - init_pose.x;
		float yt = pose.data[1].w - init_pose.y;
		float zt = pose.data[2].w - init_pose.z;

		timings[1] = tock();

		kfusion.preprocessing(inputDepth, inputSize);

		timings[2] = tock();

		bool tracked = kfusion.tracking(camera, config.icp_threshold,
			config.tracking_rate, frame);

		timings[3] = tock();

		bool integrated =
			kfusion.integration(camera, config.integration_rate, config.mu, frame);

		timings[4] = tock();

		bool raycast = kfusion.raycasting(camera, config.mu, frame);

		timings[5] = tock();

		pose = kfusion.getPose();

		for (int i = 0; i < 4; ++i)
			p_pose[i] = (float*)&pose.data[i];

		save_pose(frame, p_pose, false);

		kfusion.renderDepth(depthRender, computationSize);
		kfusion.renderTrack(trackRender, computationSize);
		kfusion.renderVolume(volumeRender, computationSize, frame,
			config.rendering_rate, camera, 0.75 * config.mu);

		write_bitmap(volume_render_bmp(frame), computationSize.x, computationSize.y, 4, (uint8_t*)volumeRender);
		write_bitmap(track_render_bmp(frame), computationSize.x, computationSize.y, 4, (uint8_t*)trackRender);
		write_bitmap(depth_render_bmp(frame), computationSize.x, computationSize.y, 4, (uint8_t*)depthRender);

		if ((frame % 20) == 0) {
			save_tsdf(frame, p_tsdf, voxel_count);
			save_vertex_normal(frame,p_vertex, p_normal,pixels);
		}

		timings[6] = tock();

		*logstream << frame << "\t" << timings[1] - timings[0]
			<< "\t"                                   //  acquisition
			<< timings[2] - timings[1] << "\t"        //  preprocessing
			<< timings[3] - timings[2] << "\t"        //  tracking
			<< timings[4] - timings[3] << "\t"        //  integration
			<< timings[5] - timings[4] << "\t"        //  raycasting
			<< timings[6] - timings[5] << "\t"        //  rendering
			<< timings[5] - timings[1] << "\t"        //  computation
			<< timings[6] - timings[0] << "\t"        //  total
			<< xt << "\t" << yt << "\t" << zt << "\t" //  X,Y,Z
			<< tracked << "        \t"
			<< integrated // tracked and integrated flags
			<< std::endl;

		frame++;

		timings[0] = tock();
	}
	// ==========     DUMP VOLUME      =========

	if (config.dump_volume_file != "") {
		kfusion.dumpVolume(config.dump_volume_file);
	}

	//  =========  FREE BASIC BUFFERS  =========

	free(inputDepth);
	free(depthRender);
	free(trackRender);
	free(volumeRender);
	return 0;
}
