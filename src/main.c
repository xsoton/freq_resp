#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <argp.h>
#include <error.h>
#include <gpib/ib.h>
#include "gpib.h"

// === [DATE] ===
struct tm start_time_struct;

// === [ARGUMENTS] ===
const char *argp_program_version = "fr 0.1";
const char *argp_program_bug_address = "<killingrain@gmail.com>";
static char doc[] =
	"FR -- a program for measuring frequency responce curve\v"
	"TODO: This part of the documentation comes *after* the options; "
	"note that the text is automatically filled, but it's possible "
	"to force a line-break, e.g.\n<-- here.";
static char args_doc[] = "SAMPLE_NAME";

// The options we understand
static struct argp_option options[] =
{
	{0,0,0,0, "Parameters:", 0},
	{"V_rms"   , 'v', "double", 0, "RMS value of sine, V            (0.004 - 5.000, step 0.002, default  0.004)", 0},
	{0,0,0,0, "Required:", 0},
	// {"Rf"      , 'r', "double", 0, "Feedback resistance of TIA, Ohm (1.0e3 - 1.0e6)", 0},
	{"Tms"     , 't', "double", 0, "Scanning delay time, s          (1.0 - 10.0)", 0},
	{0}
};

// parse arguments
struct arguments
{
	int    sample_name_flag;
	char  *sample_name;
	double V_rms;
	// int    Rf_flag;
	// double Rf;
	int    Tms_flag;
	double Tms;
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	struct arguments *a = state->input;
	double t;

	switch (key)
	{
		case 'v':
			t = atof(arg);
			if ((t < 0.004) || (t > 5.000))
			{
				fprintf(stderr, "# E: <V_rms> is out of range. See \"fr --help\"\n");
				return ARGP_ERR_UNKNOWN;
			}
			a->V_rms = t;
			break;

		// case 'r':
		// 	t = atof(arg);
		// 	if ((t < 1.0e3) || (t > 1.0e6))
		// 	{
		// 		fprintf(stderr, "# E: <Rf> is out of range. See \"fr --help\"\n");
		// 		return ARGP_ERR_UNKNOWN;
		// 	}
		// 	a->Rf = t;
		// 	a->Rf_flag = 1;
		// 	break;

		case 't':
			t = atof(arg);
			if ((t < 1.0) || (t > 10.0))
			{
				fprintf(stderr, "# E: <Tms> is out of range. See \"fr --help\"\n");
				return ARGP_ERR_UNKNOWN;
			}
			a->Tms = t;
			a->Tms_flag = 1;
			break;

		case ARGP_KEY_ARG:
			a->sample_name = arg;
			a->sample_name_flag = 1;
			break;

		case ARGP_KEY_NO_ARGS:
			fprintf(stderr, "# E: <sample_name> has not specified. See \"fr --help\"\n");
			a->sample_name_flag = 0;
			//argp_usage (state);
			return ARGP_ERR_UNKNOWN;

		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

// === [GPIB] ===
#define BOARD_GPIB_NAME "GPIB-USB-HS"
#define SR830_GPIB_NAME "SR830"

// === [SOURCE] ===
#define F_START 100.0
#define F_STOP  102000.0
#define F_N     100

// === threads ====
static void *commander(void *);
static void *worker(void *);

// === utils ===
static int get_run();
static void set_run(int run_new);
static double get_time();

// === global variables
static char dir_str[200];
static pthread_rwlock_t run_lock;
static int run;
static char filename_fr[250];
struct arguments arg = {0};

// #define DEBUG

// === program entry point
int main(int argc, char **argv)
{
	int ret = 0;
	int status;

	time_t start_time;
	// struct tm start_time_struct;

	pthread_t t_commander;
	pthread_t t_worker;

	// === parse input parameters
	arg.sample_name_flag = 0;
	arg.sample_name      = NULL;
	arg.V_rms            = 0.004;
	// arg.Rf_flag          = 0;
	// arg.Rf               = 0.0;
	arg.Tms_flag         = 0;
	arg.Tms              = 0.0;

	status = argp_parse(&argp, argc, argv, 0, 0, &arg);
	if ((status               != 0) ||
		(arg.sample_name_flag != 1) ||
		// (arg.Rf_flag          != 1) ||
		(arg.Tms_flag         != 1))
	{
		fprintf(stderr, "# E: Error while parsing. See \"fr --help\"\n");
		ret = -1;
		goto main_exit;
	}

	#ifdef DEBUG
	fprintf(stderr, "sample_name_flag = %d\n" , arg.sample_name_flag);
	fprintf(stderr, "sample_name      = %s\n" , arg.sample_name);
	fprintf(stderr, "V_rms            = %le\n", arg.V_rms);
	// fprintf(stderr, "Rf_flag          = %d\n" , arg.Rf_flag);
	// fprintf(stderr, "Rf               = %le\n", arg.Rf);
	fprintf(stderr, "Tms_flag         = %d\n" , arg.Tms_flag);
	fprintf(stderr, "Tms              = %le\n", arg.Tms);
	#endif

	// === get start time of experiment ===
	start_time = time(NULL);
	localtime_r(&start_time, &start_time_struct);

	// === we need actual information w/o buffering
	setlinebuf(stdout);
	setlinebuf(stderr);

	// === initialize run state variable
	pthread_rwlock_init(&run_lock, NULL);
	run = 1;

	// === create dirictory in "20191012_153504_<experiment_name>" format
	snprintf(dir_str, 200, "%04d-%02d-%02d_%02d-%02d-%02d_%s",
		start_time_struct.tm_year + 1900,
		start_time_struct.tm_mon + 1,
		start_time_struct.tm_mday,
		start_time_struct.tm_hour,
		start_time_struct.tm_min,
		start_time_struct.tm_sec,
		arg.sample_name
	);
	status = mkdir(dir_str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	if (status == -1)
	{
		fprintf(stderr, "# E: unable to create experiment directory (%s)\n", strerror(errno));
		ret = -2;
		goto main_exit;
	}

	// === create file names
	snprintf(filename_fr, 250, "%s/fr.dat", dir_str);
	// printf("filename_fr \"%s\"\n", filename_fr);

	// === now start threads
	pthread_create(&t_commander, NULL, commander, NULL);
	pthread_create(&t_worker, NULL, worker, NULL);

	// === and wait ...
	pthread_join(t_worker, NULL);

	// === cancel commander thread becouse we don't need it anymore
	// === and wait for cancelation finish
	pthread_cancel(t_commander);
	pthread_join(t_commander, NULL);

	fprintf(stdout, "\r\n");

	main_exit:
	return ret;
}

// === commander function
static void *commander(void *a)
{
	(void) a;

	char str[100];
	char *s;
	int ccount;

	while(get_run())
	{
		fprintf(stdout, "> ");

		s = fgets(str, 100, stdin);
		if (s == NULL)
		{
			fprintf(stderr, "# E: Exit\n");
			set_run(0);
			break;
		}

		switch(str[0])
		{
			case 'h':
				printf(
					"Help:\n"
					"\th -- this help;\n"
					"\tq -- exit the program;\n");
				break;
			case 'q':
				set_run(0);
				break;
			default:
				ccount = strlen(str)-1;
				fprintf(stderr, "# E: Unknown command (%.*s)\n", ccount, str);
				break;
		}
	}

	return NULL;
}

// === worker function
static void *worker(void *a)
{
	(void) a;

	int r;
	short int rs;
	char rb;

	int lia, hs; // lock-in

	int    fr_index;
	double fr_time;
	double I_rms, theta;

	double freq;

	double mp; // multiplicator

	uint8_t LIAS = 0, SENS = 0;

	FILE  *fr_fp;
	FILE  *gp;
	char   buf[300];

	r = gpib_open(BOARD_GPIB_NAME);
	if(r == -1)
	{
		fprintf(stderr, "# E: Unable to open gpib board (%d)\n", r);
		goto worker_hs_ibfind;
	}
	hs = r;

	r = gpib_open(SR830_GPIB_NAME);
	if(r == -1)
	{
		fprintf(stderr, "# E: Unable to open SR830 (%d)\n", r);
		goto worker_lia_ibfind;
	}
	lia = r;

	gpib_write(lia, "OUTX 1");     // select GPIB interface

	// reset status registers
	gpib_write(lia, "*SRE 0");
	gpib_write(lia, "*ESE 0");
	gpib_write(lia, "ERRE 0");
	gpib_write(lia, "LIAE 0");
	gpib_write(lia, "*CLS");

	// gpib_write(lia, "LIAE 0,1");              // check overload Input/Amplifier
	// gpib_write(lia, "LIAE 1,1");              // check overload TC filter
	// gpib_write(lia, "LIAE 2,1");              // check overload Output
	// gpib_write(lia, "*SRE 3,1");              // enable LIA events
	gpib_write(lia, "*SRE 6,1");              // enable SRQ

	gpib_write(lia, "FMOD 1");                // internal source
	gpib_write(lia, "HARM 1");                // 1st harmonic
	gpib_write(lia, "PHAS 0.0");              // ph = 0.0 degree
	gpib_write(lia, "FREQ 100");              // f = 100 Hz
	gpib_print(lia, "SLVL %.3lf", arg.V_rms); // RMS of sine
	usleep(500000);

	gpib_write(lia, "ISRC 2");                // input I(1M)
	gpib_write(lia, "IGND 0");                // float ground
	gpib_write(lia, "ICPL 0");                // AC coupling
	gpib_write(lia, "ILIN 3");                // 50 Hz and 100 Hz notch filters

	gpib_write(lia, "SENS 26");               // sensitivity is 1 uA
	gpib_write(lia, "RMOD 1");                // normal reserve mode
	gpib_write(lia, "OFLT 6");                // 10 ms is time constant
	gpib_write(lia, "OFSL 3");                // 24 dB/oct
	gpib_write(lia, "SYNC 1");                // synchtonous filter
	usleep(500000);

	gpib_write(lia, "DDEF 1,1,0"); // CH1 display is R
	gpib_write(lia, "DDEF 2,1,0"); // CH2 display is theta

	// auto gain
	gpib_write(lia, "AGAN");
	gpib_write(lia, "*SRE 1,1");
	WaitSRQ(hs, &rs); //fprintf(stderr, "WaitSRQ = %d\n", rs);
	ibrsp(lia, &rb);  //fprintf(stderr, "ibrsp = 0x%0x\n", rb);
	gpib_write(lia, "*SRE 1,0");
	gpib_write(lia, "*STB?"); gpib_read(lia, buf, 300); //fprintf(stderr, "STB  = %u\n", (uint8_t) atoi(buf));
	gpib_write(lia, "LIAS?"); gpib_read(lia, buf, 300); //fprintf(stderr, "LIAS = %u\n", (uint8_t) atoi(buf));

	// === create vac file
	fr_fp = fopen(filename_fr, "w+");
	if(fr_fp == NULL)
	{
		fprintf(stderr, "# E: Unable to open file \"%s\" (%s)\n", filename_fr, strerror(ferror(fr_fp)));
		goto worker_vac_fopen;
	}
	setlinebuf(fr_fp);

	// === write vac header
	r = fprintf(fr_fp,
		"# Measuring of frequency responce\n"
		"# I vs F\n"
		"# Date: %04d.%02d.%02d %02d:%02d:%02d\n"
		"# Start parameters:\n"
		"#   sample_name = %s\n"
		"#   V_rms       = %le\n"
		// "#   Rf          = %le\n"
		"#   Tms         = %le\n"
		"# 1: index\n"
		"# 2: time, s\n"
		"# 3: frequency, Hz\n"
		"# 4: RMS voltage, V\n"
		"# 5: RMS current, A\n"
		"# 6: theta, degree\n",
		start_time_struct.tm_year + 1900,
		start_time_struct.tm_mon + 1,
		start_time_struct.tm_mday,
		start_time_struct.tm_hour,
		start_time_struct.tm_min,
		start_time_struct.tm_sec,
		arg.sample_name,
		arg.V_rms,
		// arg.Rf,
		arg.Tms
	);
	if(r < 0)
	{
		fprintf(stderr, "# E: Unable to print to file \"%s\" (%s)\n", filename_fr, strerror(r));
		goto worker_vac_header;
	}

	// === open gnuplot
	snprintf(buf, 300, "gnuplot > %s/gnuplot.log 2>&1", dir_str);
	gp = popen(buf, "w");
	if (gp == NULL)
	{
		fprintf(stderr, "# E: unable to open gnuplot pipe (%s)\n", strerror(errno));
		goto worker_gp_popen;
	}
	setlinebuf(gp);

	// === prepare gnuplot
	r = fprintf(gp,
		"set term qt noraise\n"
		"set xzeroaxis lt -1\n"
		"set yzeroaxis lt -1\n"
		"set grid\n"
		"set key right bottom\n"
		// "set xrange [%le:%le]\n"
		"set log x\n"
		"set log y\n"
		"set xlabel \"Frequency, Hz\"\n"
		"set ylabel \"RMS current, A\"\n"
		"set format y \"%%.3s%%c\"\n"
	);
	if(r < 0)
	{
		fprintf(stderr, "# E: Unable to print to gp (%s)\n", strerror(r));
		goto worker_gp_settings;
	}

	// === let the action begins!
	fr_index = 0;

	while(get_run())
	{
		mp = pow(10, 1 + (fr_index / (F_N-(F_N/10))));

		freq = mp + (fr_index % (F_N-(F_N/10))) * mp * (10.0 / F_N);
		if (freq > F_STOP)
		{
			set_run(0);
			break;
		}

		gpib_print(lia, "FREQ %.0lf", freq);

		usleep(arg.Tms * 1e6);

		// auto gain
		while(get_run())
		{
			gpib_write(lia, "LIAS?");
			gpib_read(lia, buf, 300);
			LIAS = (uint8_t) atoi(buf);
			// fprintf(stderr, "LIAS = %u\n", LIAS);
			if (LIAS & 0x07)
			{
				gpib_write(lia, "SENS?");
				gpib_read(lia, buf, 300);
				SENS = (uint8_t) atoi(buf);
				// fprintf(stderr, "SENS = %u\n", SENS);
				if (SENS < 26)
				{
					SENS += 1;
					gpib_print(lia, "SENS %u", SENS);
					sleep(2);
					gpib_write(lia, "LIAS?");
					gpib_read(lia, buf, 300);
				}
			}
			else break;
		}

		fr_time = get_time();
		if (fr_time < 0)
		{
			fprintf(stderr, "# E: Unable to get time\n");
			set_run(0);
			break;
		}

		gpib_write(lia, "SNAP? 3,4");
		gpib_read(lia, buf, 300);
		sscanf(buf, "%lf,%lf", &I_rms, &theta);

		// I_rms /= arg.Rf;

		r = fprintf(fr_fp, "%d\t%le\t%le\t%le\t%le\t%le\n",
			fr_index,
			fr_time,
			freq,
			arg.V_rms,
			I_rms,
			theta
		);
		if(r < 0)
		{
			fprintf(stderr, "# E: Unable to print to file \"%s\" (%s)\n", filename_fr, strerror(r));
			set_run(0);
			break;
		}

		r = fprintf(gp,
			"set title \"i = %d, t = %.3lf s\"\n"
			"plot \"%s\" u 3:5 w l lw 1 title \"V = %.3lf V, I = %le A, theta = %lf\"\n",
			fr_index,
			fr_time,
			filename_fr,
			arg.V_rms,
			I_rms,
			theta
		);
		if(r < 0)
		{
			fprintf(stderr, "# E: Unable to print to gp (%s)\n", strerror(r));
			set_run(0);
			break;
		}

		fr_index++;
	}

	gpib_write(lia, "*SRE 6,0");   // disable SRQ
	gpib_write(lia, "FREQ 100");   // f = 100 Hz
	gpib_write(lia, "SLVL 0.004"); // RMS of sine
	sleep(1);

	r = fprintf(gp, "exit;\n");
	if(r < 0)
	{
		fprintf(stderr, "# E: Unable to print to gp (%s)\n", strerror(r));
	}

	worker_gp_settings:

	r = pclose(gp);
	if (r == -1)
	{
		fprintf(stderr, "# E: Unable to close gnuplot pipe (%s)\n", strerror(errno));
	}
	worker_gp_popen:


	worker_vac_header:

	r = fclose(fr_fp);
	if (r == EOF)
	{
		fprintf(stderr, "# E: Unable to close file \"%s\" (%s)\n", filename_fr, strerror(errno));
	}
	worker_vac_fopen:

	gpib_close(lia);
	worker_lia_ibfind:
	worker_hs_ibfind:

	return NULL;
}

// === utils
static int get_run()
{
	int run_local;
	pthread_rwlock_rdlock(&run_lock);
		run_local = run;
	pthread_rwlock_unlock(&run_lock);
	return run_local;
}

static void set_run(int run_new)
{
	pthread_rwlock_wrlock(&run_lock);
		run = run_new;
	pthread_rwlock_unlock(&run_lock);
}

static double get_time()
{
	static int first = 1;
	static struct timeval t_first = {0};
	struct timeval t = {0};
	double ret;
	int r;

	if (first == 1)
	{
		r = gettimeofday(&t_first, NULL);
		if (r == -1)
		{
			fprintf(stderr, "# E: unable to get time (%s)\n", strerror(errno));
			ret = -1;
		}
		else
		{
			ret = 0.0;
			first = 0;
		}
	}
	else
	{
		r = gettimeofday(&t, NULL);
		if (r == -1)
		{
			fprintf(stderr, "# E: unable to get time (%s)\n", strerror(errno));
			ret = -2;
		}
		else
		{
			ret = (t.tv_sec - t_first.tv_sec) * 1e6 + (t.tv_usec - t_first.tv_usec);
			ret /= 1e6;
		}
	}

	return ret;
}
