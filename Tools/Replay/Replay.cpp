/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_Common.h>
#include <AP_Progmem.h>
#include <AP_Param.h>
#include <StorageManager.h>
#include <AP_Math.h>
#include <AP_HAL.h>
#include <AP_HAL_AVR.h>
#include <AP_HAL_SITL.h>
#include <AP_HAL_Linux.h>
#include <AP_HAL_Empty.h>
#include <AP_ADC.h>
#include <AP_Declination.h>
#include <AP_ADC_AnalogSource.h>
#include <Filter.h>
#include <AP_Buffer.h>
#include <AP_Airspeed.h>
#include <AP_Vehicle.h>
#include <AP_Notify.h>
#include <DataFlash.h>
#include <GCS_MAVLink.h>
#include <AP_GPS.h>
#include <AP_AHRS.h>
#include <SITL.h>
#include <AP_Compass.h>
#include <AP_Baro.h>
#include <AP_InertialSensor.h>
#include <AP_InertialNav.h>
#include <AP_NavEKF.h>
#include <AP_Mission.h>
#include <AP_Rally.h>
#include <AP_BattMonitor.h>
#include <AP_Terrain.h>
#include <AP_OpticalFlow.h>
#include <Parameters.h>
#include <AP_SerialManager.h>
#include <RC_Channel.h>
#include <AP_RangeFinder.h>
#include <stdio.h>
#include <errno.h>
#include <fenv.h>
#include <VehicleType.h>
#include <getopt.h> // for optind only
#include <utility/getopt_cpp.h>
#include <MsgHandler.h>

#ifndef INT16_MIN
#define INT16_MIN -32768
#define INT16_MAX 32767
#endif

#include "LogReader.h"
#include "DataFlashFileReader.h"

#define streq(x, y) (!strcmp(x, y))

const AP_HAL::HAL& hal = AP_HAL_BOARD_DRIVER;

class Replay {
public:
    void setup();
    void loop();

    Replay() : filename("log.bin") { }

private:
    const char *filename;

    Parameters g;

    AP_InertialSensor ins;
    AP_Baro barometer;
    AP_GPS gps;
    Compass compass;
    RangeFinder rng;
    NavEKF EKF{&ahrs, barometer, rng};
    AP_AHRS_NavEKF ahrs {ins, barometer, gps, rng, EKF};
    AP_InertialNav_NavEKF inertial_nav{ahrs};
    AP_Vehicle::FixedWing aparm;
    AP_Airspeed airspeed{aparm};
    DataFlash_File dataflash{"logs"};

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    SITL sitl;
#endif

    LogReader logreader{ahrs, ins, barometer, compass, gps, airspeed, dataflash};

    FILE *plotf;
    FILE *plotf2;
    FILE *ekf1f;
    FILE *ekf2f;
    FILE *ekf3f;
    FILE *ekf4f;

    bool done_parameters;
    bool done_baro_init;
    bool done_home_init;
    uint16_t update_rate = 0;
    int32_t arm_time_ms = -1;
    bool ahrs_healthy;
    bool have_imu2 = false;
    bool have_imt = false;
    bool have_imt2 = false;
    bool have_fram = false;
    bool use_imt = true;

    void _parse_command_line(uint8_t argc, char * const argv[]);

    uint8_t num_user_parameters;
    struct {
        char name[17];
        float value;
    } user_parameters[100];

    // setup the var_info table
    AP_Param param_loader{var_info};

    static const AP_Param::Info var_info[];

    void load_parameters(void);
    void usage(void);
    void set_user_parameters(void);
    void read_sensors(const char *type);
    
};

static const struct LogStructure log_structure[] PROGMEM = {
    LOG_COMMON_STRUCTURES
};

static Replay replay;

#define GSCALAR(v, name, def) { replay.g.v.vtype, name, Parameters::k_param_ ## v, &replay.g.v, {def_value : def} }
#define GOBJECT(v, name, class) { AP_PARAM_GROUP, name, Parameters::k_param_ ## v, &replay.v, {group_info : class::var_info} }
#define GOBJECTN(v, pname, name, class) { AP_PARAM_GROUP, name, Parameters::k_param_ ## pname, &replay.v, {group_info : class::var_info} }

const AP_Param::Info Replay::var_info[] PROGMEM = {
    GSCALAR(dummy,         "_DUMMY", 0),

    // barometer ground calibration. The GND_ prefix is chosen for
    // compatibility with previous releases of ArduPlane
    // @Group: GND_
    // @Path: ../libraries/AP_Baro/AP_Baro.cpp
    GOBJECT(barometer, "GND_", AP_Baro),

    // @Group: INS_
    // @Path: ../libraries/AP_InertialSensor/AP_InertialSensor.cpp
    GOBJECT(ins,                    "INS_", AP_InertialSensor),

    // @Group: AHRS_
    // @Path: ../libraries/AP_AHRS/AP_AHRS.cpp
    GOBJECT(ahrs,                   "AHRS_",    AP_AHRS),

    // @Group: ARSPD_
    // @Path: ../libraries/AP_Airspeed/AP_Airspeed.cpp
    GOBJECT(airspeed,                               "ARSPD_",   AP_Airspeed),

    // @Group: EKF_
    // @Path: ../libraries/AP_NavEKF/AP_NavEKF.cpp
    GOBJECTN(EKF, NavEKF, "EKF_", NavEKF),

    // @Group: COMPASS_
    // @Path: ../libraries/AP_Compass/AP_Compass.cpp
    GOBJECT(compass, "COMPASS_", Compass),

    AP_VAREND
};

void Replay::load_parameters(void)
{
    if (!AP_Param::check_var_info()) {
        hal.scheduler->panic(PSTR("Bad parameter table"));
    }
}

void Replay::usage(void)
{
    ::printf("Options:\n");
    ::printf("\t--rate RATE        set IMU rate in Hz\n");
    ::printf("\t--parm NAME=VALUE  set parameter NAME to VALUE\n");
    ::printf("\t--accel-mask MASK  set accel mask (1=accel1 only, 2=accel2 only, 3=both)\n");
    ::printf("\t--gyro-mask MASK   set gyro mask (1=gyro1 only, 2=gyro2 only, 3=both)\n");
    ::printf("\t--arm-time time    arm at time (milliseconds)\n");
    ::printf("\t--no-imt           don't use IMT data\n");
}

void Replay::_parse_command_line(uint8_t argc, char * const argv[])
{
    const struct GetOptLong::option options[] = {
        {"rate",            true,   0, 'r'},
        {"parm",            true,   0, 'p'},
        {"param",           true,   0, 'p'},
        {"help",            false,  0, 'h'},
        {"accel-mask",      true,   0, 'a'},
        {"gyro-mask",       true,   0, 'g'},
        {"arm-time",        true,   0, 'A'},
        {"no-imt",          false,  0, 'n'},
        {0, false, 0, 0}
    };

    GetOptLong gopt(argc, argv, "r:p:ha:g:A:", options);
    gopt.optind = optind;

    int opt;
    while ((opt = gopt.getoption()) != -1) {
		switch (opt) {
        case 'h':
            usage();
            exit(0);

        case 'r':
			update_rate = strtol(gopt.optarg, NULL, 0);
            break;

        case 'g':
            logreader.set_gyro_mask(strtol(gopt.optarg, NULL, 0));
            break;

        case 'a':
            logreader.set_accel_mask(strtol(gopt.optarg, NULL, 0));
            break;

        case 'A':
            arm_time_ms = strtol(gopt.optarg, NULL, 0);
            break;

        case 'n':
            use_imt = false;
            logreader.set_use_imt(use_imt);
            break;

        case 'p':
            const char *eq = strchr(gopt.optarg, '=');
            if (eq == NULL) {
                ::printf("Usage: -p NAME=VALUE\n");
                exit(1);
            }
            memset(user_parameters[num_user_parameters].name, '\0', 16);
            strncpy(user_parameters[num_user_parameters].name, gopt.optarg, eq-gopt.optarg);
            user_parameters[num_user_parameters].value = atof(eq+1);
            num_user_parameters++;
            if (num_user_parameters >= sizeof(user_parameters)/sizeof(user_parameters[0])) {
                ::printf("Too many user parameters\n");
                exit(1);
            }
            break;
        }
    }

	argv += gopt.optind;
	argc -= gopt.optind;

    if (argc > 0) {
        filename = argv[0];
    }
}

class IMU2Counter : public DataFlashFileReader {
public:
    IMU2Counter() {}
    bool handle_log_format_msg(const struct log_Format &f);
    bool handle_msg(const struct log_Format &f, uint8_t *msg);

    uint64_t last_imu2_timestamp;
private:
    MsgHandler *handler;
};
bool IMU2Counter::handle_log_format_msg(const struct log_Format &f) {
    if (!strncmp(f.name,"IMU2",4)) {
        // an IMU2 message
        handler = new MsgHandler(f);
    }

    return true;
};
bool IMU2Counter::handle_msg(const struct log_Format &f, uint8_t *msg) {
    if (strncmp(f.name,"IMU2",4)) {
        // not an IMU2 message
        return true;
    }

    if (handler->field_value(msg, "TimeUS", last_imu2_timestamp)) {
//        ::printf("Found timestamp %ld\n", last_imu2_timestamp);
    } else if (handler->field_value(msg, "TimeMS", last_imu2_timestamp)) {
//        ::printf("Found millisecond timestamp %ld\n", last_imu2_timestamp);
        last_imu2_timestamp *= 1000;
    } else {
        ::printf("Unable to find timestamp in IMU2 message");
    }
    return true;
}

int find_update_rate(const char *filename) {
    IMU2Counter reader;
    if (!reader.open_log(filename)) {
        perror(filename);
        exit(1);
    }
    int samplecount = 0;
    uint64_t prev = 0;
    uint64_t samplesum = 0;
    prev = 0;
    while (samplecount < 10) {
        char type[5];
        if (!reader.update(type)) {
            break;
        }
        if (streq(type, "IMU2")) {
            if (prev == 0) {
                prev = reader.last_imu2_timestamp;
            } else {
                samplecount++;
                samplesum += reader.last_imu2_timestamp - prev;
                prev = reader.last_imu2_timestamp;
            }
        }
    }
    if (samplecount < 10) {
        ::printf("Unable to determine log rate - insufficient IMU2 messages?!");
        exit(1);
    }

    float rate = 1000000/int(samplesum/samplecount);
    if (abs(rate - 50) < 5) {
        return 50;
    }
    if (abs(rate - 100) < 10) {
        return 100;
    }
    if (abs(rate - 200) < 10) {
        return 200;
    }
    if (abs(rate - 400) < 20) { // I have a log which is 10 off...
        return 400;
    }
    ::printf("Unable to determine log rate - %f matches no rate\n", rate);
    exit(1);
}

void Replay::setup()
{
    ::printf("Starting\n");

    uint8_t argc;
    char * const *argv;

    hal.util->commandline_arguments(argc, argv);

    _parse_command_line(argc, argv);

    hal.console->printf("Processing log %s\n", filename);

    if (update_rate == 0) {
        update_rate = find_update_rate(filename);
    }

    hal.console->printf("Using an update rate of %u Hz\n", update_rate);

    load_parameters();

    if (!logreader.open_log(filename)) {
        perror(filename);
        exit(1);
    }

    dataflash.Init(log_structure, sizeof(log_structure)/sizeof(log_structure[0]));
    dataflash.StartNewLog();

    logreader.wait_type("GPS");
    logreader.wait_type("IMU");
    logreader.wait_type("GPS");
    logreader.wait_type("IMU");

    feenableexcept(FE_INVALID | FE_OVERFLOW);

    ahrs.set_compass(&compass);
    ahrs.set_fly_forward(true);
    ahrs.set_wind_estimation(true);
    ahrs.set_correct_centrifugal(true);

    printf("Starting disarmed\n");
    hal.util->set_soft_armed(false);

    barometer.init();
    barometer.setHIL(0);
    barometer.update();
    compass.init();
    ins.set_hil_mode();

    switch (update_rate) {
    case 50:
        ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_50HZ);
        break;
    case 100:
        ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_100HZ);
        break;
    case 200:
        ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_200HZ);
        break;
    case 400:
        ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_400HZ);
        break;
    default:
        printf("Invalid update rate (%d); use 50, 100, 200 or 400\n",update_rate);
        exit(1);
    }

    plotf = fopen("plot.dat", "w");
    plotf2 = fopen("plot2.dat", "w");
    ekf1f = fopen("EKF1.dat", "w");
    ekf2f = fopen("EKF2.dat", "w");
    ekf3f = fopen("EKF3.dat", "w");
    ekf4f = fopen("EKF4.dat", "w");

    fprintf(plotf, "time SIM.Roll SIM.Pitch SIM.Yaw BAR.Alt FLIGHT.Roll FLIGHT.Pitch FLIGHT.Yaw FLIGHT.dN FLIGHT.dE FLIGHT.Alt AHR2.Roll AHR2.Pitch AHR2.Yaw DCM.Roll DCM.Pitch DCM.Yaw EKF.Roll EKF.Pitch EKF.Yaw INAV.dN INAV.dE INAV.Alt EKF.dN EKF.dE EKF.Alt\n");
    fprintf(plotf2, "time E1 E2 E3 VN VE VD PN PE PD GX GY GZ WN WE MN ME MD MX MY MZ E1ref E2ref E3ref\n");
    fprintf(ekf1f, "timestamp TimeMS Roll Pitch Yaw VN VE VD PN PE PD GX GY GZ\n");
    fprintf(ekf2f, "timestamp TimeMS AX AY AZ VWN VWE MN ME MD MX MY MZ\n");
    fprintf(ekf3f, "timestamp TimeMS IVN IVE IVD IPN IPE IPD IMX IMY IMZ IVT\n");
    fprintf(ekf4f, "timestamp TimeMS SV SP SH SMX SMY SMZ SVT OFN EFE FS DS\n");

    ahrs.set_ekf_use(true);

    ::printf("Waiting for GPS\n");
    while (!done_home_init) {
        char type[5];
        if (!logreader.update(type)) {
            break;
        }
        read_sensors(type);
        if (streq(type, "GPS") &&
            gps.status() >= AP_GPS::GPS_OK_FIX_3D && 
            done_baro_init && !done_home_init) {
            const Location &loc = gps.location();
            ::printf("GPS Lock at %.7f %.7f %.2fm time=%.1f seconds\n", 
                     loc.lat * 1.0e-7f, 
                     loc.lng * 1.0e-7f,
                     loc.alt * 0.01f,
                     hal.scheduler->millis()*0.001f);
            ahrs.set_home(loc);
            compass.set_initial_location(loc.lat, loc.lng);
            done_home_init = true;
        }
    }
}


/*
  setup user -p parameters
 */
void Replay::set_user_parameters(void)
{
    for (uint8_t i=0; i<num_user_parameters; i++) {
        if (!logreader.set_parameter(user_parameters[i].name, user_parameters[i].value)) {
            ::printf("Failed to set parameter %s to %f\n", user_parameters[i].name, user_parameters[i].value);
            exit(1);
        }
    }
}

void Replay::read_sensors(const char *type)
{
    if (!done_parameters && !streq(type,"FMT") && !streq(type,"PARM")) {
        done_parameters = true;
        set_user_parameters();
    }
    if (streq(type,"IMU2")) {
        have_imu2 = true;
    }
    if (use_imt && streq(type,"IMT")) {
        have_imt = true;
    }
    if (use_imt && streq(type,"IMT2")) {
        have_imt2 = true;
    }

    if (streq(type,"GPS")) {
        gps.update();
        if (gps.status() >= AP_GPS::GPS_OK_FIX_3D) {
            ahrs.estimate_wind();
        }
    } else if (streq(type,"MAG")) {
        compass.read();
    } else if (streq(type,"ARSP")) {
        ahrs.set_airspeed(&airspeed);
    } else if (streq(type,"BARO")) {
        barometer.update();
        if (!done_baro_init) {
            done_baro_init = true;
            ::printf("Barometer initialised\n");
            barometer.update_calibration();
        }
    } 

    bool run_ahrs = false;
    if (streq(type,"FRAM")) {
        if (!have_fram) {
            have_fram = true;
            printf("Have FRAM framing\n");
        }
        run_ahrs = true;
    }

    if (have_imt) {
        if ((streq(type,"IMT") && !have_imt2) ||
            (streq(type,"IMT2") && have_imt2)) {
            run_ahrs = true;
        }
    }

    // special handling of IMU messages as these trigger an ahrs.update()
    if (!have_fram && 
        !have_imt &&
        ((streq(type,"IMU") && !have_imu2) || (streq(type, "IMU2") && have_imu2))) {
        run_ahrs = true;
    }
    if (run_ahrs) {
        ahrs.update();
        if (ahrs.get_home().lat != 0) {
            inertial_nav.update(ins.get_delta_time());
        }
        dataflash.Log_Write_EKF(ahrs,false);
        dataflash.Log_Write_AHRS2(ahrs);
        dataflash.Log_Write_POS(ahrs);
        if (ahrs.healthy() != ahrs_healthy) {
            ahrs_healthy = ahrs.healthy();
            printf("AHRS health: %u at %lu\n", 
                   (unsigned)ahrs_healthy,
                   (unsigned long)hal.scheduler->millis());
        }
    }
}

void Replay::loop()
{
    while (true) {
        char type[5];

        if (arm_time_ms >= 0 && hal.scheduler->millis() > (uint32_t)arm_time_ms) {
            if (!hal.util->get_soft_armed()) {
                hal.util->set_soft_armed(true);
                ::printf("Arming at %u ms\n", (unsigned)hal.scheduler->millis());
            }
        }

        if (!logreader.update(type)) {
            ::printf("End of log at %.1f seconds\n", hal.scheduler->millis()*0.001f);
            fclose(plotf);
            exit(0);
        }
        read_sensors(type);

        if (streq(type,"ATT")) {
            Vector3f ekf_euler;
            Vector3f velNED;
            Vector3f posNED;
            Vector3f gyroBias;
            float accelWeighting;
            float accelZBias1;
            float accelZBias2;
            Vector3f windVel;
            Vector3f magNED;
            Vector3f magXYZ;
            Vector3f DCM_attitude;
            Vector3f ekf_relpos;
            Vector3f velInnov;
            Vector3f posInnov;
            Vector3f magInnov;
            float    tasInnov;
            float velVar;
            float posVar;
            float hgtVar;
            Vector3f magVar;
            float tasVar;
            Vector2f offset;
            uint8_t faultStatus;

            const Matrix3f &dcm_matrix = ahrs.AP_AHRS_DCM::get_dcm_matrix();
            dcm_matrix.to_euler(&DCM_attitude.x, &DCM_attitude.y, &DCM_attitude.z);
            EKF.getEulerAngles(ekf_euler);
            EKF.getVelNED(velNED);
            EKF.getPosNED(posNED);
            EKF.getGyroBias(gyroBias);
            EKF.getIMU1Weighting(accelWeighting);
            EKF.getAccelZBias(accelZBias1, accelZBias2);
            EKF.getWind(windVel);
            EKF.getMagNED(magNED);
            EKF.getMagXYZ(magXYZ);
            EKF.getInnovations(velInnov, posInnov, magInnov, tasInnov);
            EKF.getVariances(velVar, posVar, hgtVar, magVar, tasVar, offset);
            EKF.getFilterFaults(faultStatus);
            EKF.getPosNED(ekf_relpos);
            Vector3f inav_pos = inertial_nav.get_position() * 0.01f;
            float temp = degrees(ekf_euler.z);

            if (temp < 0.0f) temp = temp + 360.0f;
            fprintf(plotf, "%.3f %.1f %.1f %.1f %.2f %.1f %.1f %.1f %.2f %.2f %.2f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                    hal.scheduler->millis() * 0.001f,
                    logreader.get_sim_attitude().x,
                    logreader.get_sim_attitude().y,
                    logreader.get_sim_attitude().z,
                    barometer.get_altitude(),
                    logreader.get_attitude().x,
                    logreader.get_attitude().y,
                    wrap_180_cd(logreader.get_attitude().z*100)*0.01f,
                    logreader.get_inavpos().x,
                    logreader.get_inavpos().y,
                    logreader.get_relalt(),
                    logreader.get_ahr2_attitude().x,
                    logreader.get_ahr2_attitude().y,
                    wrap_180_cd(logreader.get_ahr2_attitude().z*100)*0.01f,
                    degrees(DCM_attitude.x),
                    degrees(DCM_attitude.y),
                    degrees(DCM_attitude.z),
                    degrees(ekf_euler.x),
                    degrees(ekf_euler.y),
                    degrees(ekf_euler.z),
                    inav_pos.x,
                    inav_pos.y,
                    inav_pos.z,
                    ekf_relpos.x,
                    ekf_relpos.y,
                    -ekf_relpos.z);
            fprintf(plotf2, "%.3f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
                    hal.scheduler->millis() * 0.001f,
                    degrees(ekf_euler.x),
                    degrees(ekf_euler.y),
                    temp,
                    velNED.x, 
                    velNED.y, 
                    velNED.z, 
                    posNED.x, 
                    posNED.y, 
                    posNED.z, 
                    60*degrees(gyroBias.x), 
                    60*degrees(gyroBias.y), 
                    60*degrees(gyroBias.z), 
                    windVel.x, 
                    windVel.y, 
                    magNED.x, 
                    magNED.y, 
                    magNED.z, 
                    magXYZ.x, 
                    magXYZ.y, 
                    magXYZ.z,
                    logreader.get_attitude().x,
                    logreader.get_attitude().y,
                    logreader.get_attitude().z);

            // define messages for EKF1 data packet
            int16_t     roll  = (int16_t)(100*degrees(ekf_euler.x)); // roll angle (centi-deg)
            int16_t     pitch = (int16_t)(100*degrees(ekf_euler.y)); // pitch angle (centi-deg)
            uint16_t    yaw   = (uint16_t)wrap_360_cd(100*degrees(ekf_euler.z)); // yaw angle (centi-deg)
            float       velN  = (float)(velNED.x); // velocity North (m/s)
            float       velE  = (float)(velNED.y); // velocity East (m/s)
            float       velD  = (float)(velNED.z); // velocity Down (m/s)
            float       posN  = (float)(posNED.x); // metres North
            float       posE  = (float)(posNED.y); // metres East
            float       posD  = (float)(posNED.z); // metres Down
            float       gyrX  = (float)(6000*degrees(gyroBias.x)); // centi-deg/min
            float       gyrY  = (float)(6000*degrees(gyroBias.y)); // centi-deg/min
            float       gyrZ  = (float)(6000*degrees(gyroBias.z)); // centi-deg/min

            // print EKF1 data packet
            fprintf(ekf1f, "%.3f %u %d %d %u %.2f %.2f %.2f %.2f %.2f %.2f %.0f %.0f %.0f\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    roll, 
                    pitch, 
                    yaw, 
                    velN, 
                    velE, 
                    velD, 
                    posN, 
                    posE, 
                    posD, 
                    gyrX,
                    gyrY,
                    gyrZ);

            // define messages for EKF2 data packet
            int8_t  accWeight  = (int8_t)(100*accelWeighting);
            int8_t  acc1  = (int8_t)(100*accelZBias1);
            int8_t  acc2  = (int8_t)(100*accelZBias2);
            int16_t windN = (int16_t)(100*windVel.x);
            int16_t windE = (int16_t)(100*windVel.y);
            int16_t magN  = (int16_t)(magNED.x);
            int16_t magE  = (int16_t)(magNED.y);
            int16_t magD  = (int16_t)(magNED.z);
            int16_t magX  = (int16_t)(magXYZ.x);
            int16_t magY  = (int16_t)(magXYZ.y);
            int16_t magZ  = (int16_t)(magXYZ.z);

            // print EKF2 data packet
            fprintf(ekf2f, "%.3f %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    accWeight, 
                    acc1, 
                    acc2, 
                    windN, 
                    windE, 
                    magN, 
                    magE, 
                    magD, 
                    magX,
                    magY,
                    magZ);

            // define messages for EKF3 data packet
            int16_t innovVN = (int16_t)(100*velInnov.x);
            int16_t innovVE = (int16_t)(100*velInnov.y);
            int16_t innovVD = (int16_t)(100*velInnov.z);
            int16_t innovPN = (int16_t)(100*posInnov.x);
            int16_t innovPE = (int16_t)(100*posInnov.y);
            int16_t innovPD = (int16_t)(100*posInnov.z);
            int16_t innovMX = (int16_t)(magInnov.x);
            int16_t innovMY = (int16_t)(magInnov.y);
            int16_t innovMZ = (int16_t)(magInnov.z);
            int16_t innovVT = (int16_t)(100*tasInnov);

            // print EKF3 data packet
            fprintf(ekf3f, "%.3f %d %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    innovVN, 
                    innovVE, 
                    innovVD, 
                    innovPN, 
                    innovPE, 
                    innovPD, 
                    innovMX, 
                    innovMY, 
                    innovMZ, 
                    innovVT);

            // define messages for EKF4 data packet
            int16_t sqrtvarV = (int16_t)(constrain_float(100*velVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarP = (int16_t)(constrain_float(100*posVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarH = (int16_t)(constrain_float(100*hgtVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMX = (int16_t)(constrain_float(100*magVar.x,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMY = (int16_t)(constrain_float(100*magVar.y,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMZ = (int16_t)(constrain_float(100*magVar.z,INT16_MIN,INT16_MAX));
            int16_t sqrtvarVT = (int16_t)(constrain_float(100*tasVar,INT16_MIN,INT16_MAX));
            int16_t offsetNorth = (int8_t)(constrain_float(offset.x,INT16_MIN,INT16_MAX));
            int16_t offsetEast = (int8_t)(constrain_float(offset.y,INT16_MIN,INT16_MAX));

            // print EKF4 data packet
            fprintf(ekf4f, "%.3f %u %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    (unsigned)hal.scheduler->millis(),
                    (int)sqrtvarV,
                    (int)sqrtvarP,
                    (int)sqrtvarH,
                    (int)sqrtvarMX, 
                    (int)sqrtvarMY, 
                    (int)sqrtvarMZ,
                    (int)sqrtvarVT,
                    (int)offsetNorth,
                    (int)offsetEast,
                    (int)faultStatus);
        }
    }
}

/*
  compatibility with old pde style build
 */
void setup(void);
void loop(void);

void setup(void)
{
    replay.setup();
}
void loop(void)
{
    replay.loop();
}

AP_HAL_MAIN();
