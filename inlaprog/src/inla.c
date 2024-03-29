
/* inla.c
 * 
 * Copyright (C) 2007-2015 Havard Rue
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The author's contact information:
 *
 *       H{\aa}vard Rue
 *       Department of Mathematical Sciences
 *       The Norwegian University of Science and Technology
 *       N-7491 Trondheim, Norway
 *       Voice: +47-7359-3533    URL  : http://www.math.ntnu.no/~hrue  
 *       Fax  : +47-7359-3524    Email: havard.rue@math.ntnu.no
 *
 */
#ifndef HGVERSION
#define HGVERSION
#endif
static const char RCSId[] = HGVERSION;

#if defined(__sun__)
#include <stdlib.h>
#endif
#if defined(__linux__)
#include <getopt.h>
#endif
#include <float.h>
#if !defined(__FreeBSD__)
#include <malloc.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if !defined(WINDOWS)
#include <sys/resource.h>
#endif

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include "GMRFLib/GMRFLib.h"
#include "GMRFLib/GMRFLibP.h"

//#include <openssl/sha.h>                                     /* Would also work with this library... */
#include "sha1.h"					       /* instead of this one */
#define INLA_SHA1					       /* use SHA1 check to reuse the mode */

#include <unistd.h>
#include <stdlib.h>
#if defined(WIN32) || defined(WINDOWS)
#include <windows.h>
#endif

#include "inla.h"
#include "spde.h"
#include "spde2.h"
#include "spde3.h"
#include "eval.h"
#include "interpol.h"
#include "re.h"
#include "ar.h"
#include "pc-priors.h"
#include "R-interface.h"

#define PREVIEW (20)
#define MODEFILENAME ".inla-mode"
#define MODEFILENAME_FMT "%02x"

#define TSTRATA_MAXTHETA (11)				       /* as given in models.R */
#define SPDE2_MAXTHETA   (100)				       /* as given in models.R */
#define SPDE3_MAXTHETA (100)				       /* as given in models.R */
#define GENERIC3_MAXTHETA (11)				       /* as given in models.R */
#define AR_MAXTHETA   (10)				       /* as given in models.R */
#define LINK_MAXTHETA (10)				       /* as given in models.R */

G_tp G = { 0, 1, INLA_MODE_DEFAULT, 4.0, 0.5, 2, 0, -1, 0, 0 };

/* 
   default values for priors
 */
#define DEFAULT_GAMMA_PRIOR_A          1.0
#define DEFAULT_GAMMA_PRIOR_B          0.00005
#define DEFAULT_NORMAL_PRIOR_PRECISION 0.001

/* 
   these are the same, just that the interface is cleaner with the `ds'
 */
#define OFFSET(idx_) ds->offset[idx_]
#define OFFSET2(idx_) mb_old->offset[idx_]
#define OFFSET3(idx_) mb->offset[idx_]

#define LINK_INIT \
	double *_link_covariates = NULL;				\
	if (ds->link_covariates) {					\
		_link_covariates = Calloc(ds->link_covariates->ncol, double); \
		GMRFLib_matrix_get_row(_link_covariates, idx, ds->link_covariates); \
	}
#define LINK_END  \
	Free(_link_covariates)

#define PREDICTOR_INVERSE_LINK(xx_)  \
	ds->predictor_invlinkfunc(xx_, MAP_FORWARD, ds->predictor_invlinkfunc_arg, _link_covariates)

#define PREDICTOR_LINK(xx_)  \
	ds->predictor_invlinkfunc(xx_, MAP_BACKWARD, ds->predictor_invlinkfunc_arg, _link_covariates)

#define PREDICTOR_INVERSE_LINK_LOGJACOBIAN(xx_)  \
	log(ABS(ds->predictor_invlinkfunc(xx_, MAP_DFORWARD, ds->predictor_invlinkfunc_arg, _link_covariates)))

#define PENALTY (-100.0)				       /* wishart3d: going over limit... */

#define READ(fd, buf, num, type)					\
	if (1) {							\
		if (num > 0) {						\
			int bytes_read;					\
			bytes_read = read(fd, (void *) buf, (size_t) ((num)*sizeof(type))); \
			assert(bytes_read == (int) ((num)*sizeof(type))); \
		}							\
	}


#define WRITE(fd, buf, num, type)					\
	if (1) {							\
		if (num > 0) {						\
			int bytes_written;				\
			bytes_written = write(fd, (void *) buf, (size_t) ((num)*sizeof(type))); \
			assert(bytes_written == (int) ((num)*sizeof(type))); \
		}							\
	}

#define WRITE_MSG(fd, Id, msg)					\
	if (1) {						\
		int _id = (Id);					\
		WRITE(fd, &_id, 1, int);			\
		char *dup_msg = GMRFLib_strdup(msg);		\
		WRITE(fd, dup_msg, strlen(dup_msg)+1, char);	\
		Free(dup_msg);					\
	}


int inla_ncpu(void)
{
#if defined(_SC_NPROCESSORS_ONLN)			       /* Linux, Solaris, AIX */
	return (int) sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__APPLE__)				       /* MacOSX */
	int count = -1;
	size_t size = sizeof(count);
	sysctlbyname("hw.ncpu", &count, &size, NULL, 0);
	return count;
#elif (defined(WIN32) || defined(WINDOWS))		       /* Windows */
	SYSTEM_INFO SystemInfo;
	GetSystemInfo(&SystemInfo);
	return SystemInfo.dwNumberOfProcessors;
#else
	return -1;
#endif
}
char *inla_fnmfix(char *name)
{
	if (!name) {
		return NULL;
	}
	int i;
	for (i = 0; i < (int) strlen(name); i++) {
		if (strncmp(name + i, " ", 1) == 0 || strncmp(name + i, "\t", 1) == 0) {
			name[i] = '-';
		}
	}
	return name;
}
int inla_mkdir(const char *dirname)
{
#if defined(WINDOWS)
	return mkdir(dirname);
#else
	return mkdir(dirname, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#endif
}
unsigned char *inla_inifile_sha1(const char *filename)
{
#if !defined(INLA_SHA1)
	{
		unsigned char *hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		hash[SHA_DIGEST_LENGTH] = '\0';

		return hash;
	}
#else

#define BUFSIZE (1024*16)
#define SCAN_FILE(fnm)							\
	{								\
		FILE *fp;						\
									\
		if (debug) {						\
			printf("\tbuild SHA1 for file %s\n", fnm);	\
		}							\
									\
		fp = fopen(fnm, "r");					\
		if (fp) {						\
			ssize_t fd = fileno(fp);			\
			while (1) {					\
				ssize_t j = read(fd, buf, (size_t)BUFSIZE); \
				if (j <= 0L )				\
					break;				\
				SHA1_Update(&c, buf, (unsigned long) j); \
			}						\
			fclose(fp);					\
		} else {						\
			if (debug) {					\
				printf("Fail to open file [%s]\n", fnm); \
			}						\
			memset(md, 0, SHA_DIGEST_LENGTH);		\
			hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char); \
			hash[SHA_DIGEST_LENGTH] = '\0';			\
			memcpy(hash, md, SHA_DIGEST_LENGTH * sizeof(unsigned char)); \
									\
			return hash;					\
		}							\
	}

	if (0) {
		/*
		 * simple check: check just the inifile 
		 */
		FILE *fp;
		unsigned char *hash = NULL;

		fp = fopen(filename, "r");
		if (fp) {
			hash = inla_fp_sha1(fp);
			fclose(fp);
		} else {
			hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
			hash[SHA_DIGEST_LENGTH] = '\0';
		}

		return hash;

	} else {
		/*
		 * this is the complete check; check the inifile and all the files referred to in it. 
		 */
		SHA_CTX c;
		dictionary *d = NULL;
		int i, debug = 0;
		unsigned char *hash = NULL, buf[BUFSIZE], md[SHA_DIGEST_LENGTH];
		map_stri fhash;

		map_stri_init(&fhash);

		memset(md, 0, SHA_DIGEST_LENGTH);
		SHA1_Init(&c);
		SCAN_FILE(filename);

		if (debug)
			printf("load %s\n", filename);
		d = iniparser_load(filename);
		for (i = 0; i < d->size; i++) {
			if (d->key[i]) {
				char *p;
				p = strchr(d->key[i], INIPARSER_SEP);
				if (p) {
					p++;
					if (debug)
						fprintf(stdout, "%20s\t[%s]\n", d->key[i], d->val[i] ? d->val[i] : "NULL");
					if (!strcasecmp("FILENAME", p)
					    || !strcasecmp("OFFSET", p)
					    || !strcasecmp("COVARIATES", p)
					    || !strcasecmp("WEIGHTS", p)
					    || !strcasecmp("CMATRIX", p)
					    || !strcasecmp("GRAPH", p)
					    || !strcasecmp("LOCATIONS", p)
					    || !strcasecmp("X", p)
					    || !strcasecmp("THETA", p)
					    || !strcasecmp("AEXT", p)
					    || !strcasecmp("EXTRACONSTRAINT", p)) {
						char *f = GMRFLib_strdup(dictionary_replace_variables(d, d->val[i]));

						/*
						 * make sure we do not read the same file twice! this is required for all the lincomb's of Finn... 
						 */
						if (map_stri_ptr(&fhash, f)) {
							if (debug)
								printf("\talready scanned file %s\n", f);
						} else {
							if (debug)
								printf("\tscan file %s\n", f);
							map_stri_set(&fhash, f, 1);
							SCAN_FILE(f);
						}
					}
				}
			}
		}
		map_stri_free(&fhash);

		SHA1_Final(&(md[0]), &c);
		hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		hash[SHA_DIGEST_LENGTH] = '\0';
		memcpy(hash, md, SHA_DIGEST_LENGTH * sizeof(unsigned char));

		return hash;
	}
#undef BUFSIZE
#endif
}
unsigned char *inla_fp_sha1(FILE * fp)
{
#define BUFSIZE (1024*16)

	if (!fp) {
		unsigned char *hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		hash[SHA_DIGEST_LENGTH] = '\0';

		return hash;
	}
#if !defined(INLA_SHA1)
	{
		unsigned char *hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		hash[SHA_DIGEST_LENGTH] = '\0';

		return hash;
	}
#else
	{
		/*
		 * return the SHA1 in a alloced string (including an extra \0).
		 * 
		 * This code is copied from crypto/sha/sha1.c in the openssl library 
		 */

		SHA_CTX c;
		unsigned char md[SHA_DIGEST_LENGTH];
		int fd;
		unsigned char buf[BUFSIZE];

		memset(md, 0, SHA_DIGEST_LENGTH);

		fd = fileno(fp);
		SHA1_Init(&c);
		while (1) {
			ssize_t i;

			i = read(fd, buf, BUFSIZE);
			if (i <= 0) {
				break;
			}
			SHA1_Update(&c, buf, (unsigned long) i);
		}
		SHA1_Final(&(md[0]), &c);

		unsigned char *hash = NULL;
		hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		hash[SHA_DIGEST_LENGTH] = '\0';
		memcpy(hash, md, SHA_DIGEST_LENGTH * sizeof(unsigned char));

		return hash;
	}
#endif
#undef BUFSIZE
}
int inla_print_sha1(FILE * fp, unsigned char *md)
{
#if !defined(INLA_SHA1)
	{
		return INLA_OK;
	}
#else
	{
		int i;

		for (i = 0; i < SHA_DIGEST_LENGTH; i++) {
			fprintf(fp, MODEFILENAME_FMT, md[i]);
		}
		fprintf(fp, "\n");

		return INLA_OK;
	}
#endif
}
double map_identity(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the idenity map-function
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return arg;
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return arg;
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return 1.0;
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_identity_scale(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the idenity map-function
	 */
	double scale = (param ? *((double *) param) : 1.0);

	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return arg * scale;
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return arg / scale;
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return scale;
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return (scale > 0 ? 1.0 : 0.0);
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_exp(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the exp-map-function
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return exp(arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return log(arg);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return exp(arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_invprobit(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the inverse probit function
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return gsl_cdf_ugaussian_P(arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return gsl_cdf_ugaussian_Pinv(arg);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return gsl_ran_ugaussian_pdf(arg);

	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_invcloglog(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the inverse cloglog function
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return exp(-exp(-arg));
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return -log(-log(arg));
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return exp(-arg) * exp(-exp(-arg));
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_beta(double x, map_arg_tp typ, void *param)
{
	/*
	 * the map for the beta parameter, which can have a lower and upper range as well. If range.low=range.high, then its interpreted as range.low = -INF and
	 * range.high = INF, ie the mapping is the identity. If range.high = INF and range.low != INF, then the mapping is range.low + exp(...).
	 */

	double *range = (double *) param;

	if (param == NULL || ISEQUAL(range[0], range[1])) {
		return map_identity(x, typ, param);
	}

	if (ISINF(range[1]) && !ISINF(range[0])) {
		switch (typ) {
		case MAP_FORWARD:
			/*
			 * extern = func(local) 
			 */
			return range[0] + exp(x);
		case MAP_BACKWARD:
			/*
			 * local = func(extern) 
			 */
			return log(x - range[0]);
		case MAP_DFORWARD:
			/*
			 * d_extern / d_local 
			 */
			return exp(x);
		case MAP_INCREASING:
			/*
			 * return 1.0 if montone increasing and 0.0 otherwise 
			 */
			return 1.0;
		default:
			abort();
		}
	} else if (ISINF(range[0]) && !ISINF(range[1])) {
		FIXME("the case: ISINF(range[0]) && !ISINF(range[1]), is not yet implemented.");
		exit(EXIT_FAILURE);
	} else {
		/*
		 * Then the mapping is
		 * 
		 * range[0] + exp(x)/(1 + exp(x)) * (range[1] - range[0]) 
		 */

		double d = range[1] - range[0], xx;

		switch (typ) {
		case MAP_FORWARD:
			return range[0] + d * exp(x) / (1.0 + exp(x));
		case MAP_BACKWARD:
			xx = (x - range[0]) / d;
			return log(xx / (1.0 - xx));
		case MAP_DFORWARD:
			return d * exp(x) / SQR(1 + exp(x));
		case MAP_INCREASING:
			return 1.0;
		default:
			assert(0 == 1);
		}
	}
	return 0.0;
}
double map_1exp(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the 1/exp-map-function
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return exp(-arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return -log(arg);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return -exp(-arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 0.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_sqrt1exp(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the sqrt(1/exp) map
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return exp(-0.5 * arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return -2.0 * log(arg);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return -0.5 * exp(-0.5 * arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 0.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_shape_svnig(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the mapping for the shape-parameters in the stochvol-nig model. shape = 1 + exp(shape_intern)
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return 1.0 + exp(arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return log(arg - 1.0);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return exp(arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_dof(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the degrees of freedom for the student-t 
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return 2.0 + exp(arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return log(arg - 2.0);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return exp(arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_dof5(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the degrees of freedom for the student-t 
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return 5.0 + exp(arg);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return log(arg - 5.0);
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return exp(arg);
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_phi(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the lag-1 correlation in the AR(1) model 
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return (2.0 * (exp((arg)) / (1.0 + exp((arg)))) - 1.0);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return log((1.0 + arg) / (1.0 - arg));
	case MAP_DFORWARD:
		/*
		 * d_extern / d_local 
		 */
		return 2.0 * exp(arg) / ((SQR(1.0 + exp(arg))));
	case MAP_INCREASING:
		/*
		 * return 1.0 if montone increasing and 0.0 otherwise 
		 */
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
double map_rho(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the correlation in the 2D iid model
	 */
	return map_phi(arg, typ, param);
}
double map_precision(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the precision variables 
	 */
	return map_exp(arg, typ, param);
}
double map_tau_laplace(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the tau variable for the laplace likelihood
	 */
	return map_exp(arg, typ, param);
}
double map_range(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the range
	 */
	return map_exp(arg, typ, param);
}
double map_alpha_weibull(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the range
	 */
	return map_exp(arg, typ, param);
}
double map_alpha_loglogistic(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the range
	 */
	return map_exp(arg, typ, param);
}
double map_alpha_weibull_cure(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the alpha-parameter in L_WEIBULL_CURE
	 */
	return map_exp(arg, typ, param);
}
double map_p_weibull_cure(double arg, map_arg_tp typ, void *param)
{
	/*
	 * the map-function for the p parameter in L_WEIBULL_CURE
	 */
	return map_invlogit(arg, typ, param);
}
double map_invlogit(double x, map_arg_tp typ, void *param)
{
	/*
	 * extern = exp(local) / (1 + exp(local)) 
	 */
	switch (typ) {
	case MAP_FORWARD:
		return exp(x) / (1.0 + exp(x));
	case MAP_BACKWARD:
		return log(x / (1.0 - x));
	case MAP_DFORWARD:
		return exp(x) / SQR(1.0 + exp(x));
	case MAP_INCREASING:
		return 1.0;
	default:
		assert(0 == 1);
	}
	return 0.0;
}
double map_probability(double x, map_arg_tp typ, void *param)
{
	/*
	 * extern = exp(local) / (1 + exp(local)) 
	 */
	switch (typ) {
	case MAP_FORWARD:
		return exp(x) / (1.0 + exp(x));
	case MAP_BACKWARD:
		return log(x / (1.0 - x));
	case MAP_DFORWARD:
		return exp(x) / SQR(1.0 + exp(x));
	case MAP_INCREASING:
		return 1.0;
	default:
		assert(0 == 1);
	}
	return 0.0;
}
double map_group_rho(double x, map_arg_tp typ, void *param)
{
	/*
	 * extern = 
	 */
	assert(param != NULL);
	int ngroups = *((int *) param);

	switch (typ) {
	case MAP_FORWARD:
		return (exp(x) - 1.0) / (exp(x) + ngroups - 1.0);
	case MAP_BACKWARD:
		return log((1.0 + (ngroups - 1.0) * x) / (1.0 - x));
	case MAP_DFORWARD:
		return (exp(x) * ngroups) / SQR(exp(x) + ngroups - 1.0);
	case MAP_INCREASING:
		return 1.0;
	default:
		assert(0 == 1);
	}
	return 0.0;
}
double map_invtan(double x, map_arg_tp typ, void *param)
{
	/*
	 * y = 2*atan(x), so that |y| <= Pi
	 */
	switch (typ) {
	case MAP_FORWARD:
		/*
		 * extern = func(local) 
		 */
		return 2.0 * atan(x);
	case MAP_BACKWARD:
		/*
		 * local = func(extern) 
		 */
		return tan(x / 2.0);
	case MAP_DFORWARD:
		return 2.0 / (1.0 + SQR(x));
	case MAP_INCREASING:
		return 1.0;
	default:
		assert(0 == 1);
	}
}
double link_this_should_not_happen(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	FIXMEE("This function is called because a wrong link function is used.");
	abort();
	return 0.0;
}
double link_probit(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	return map_invprobit(x, typ, param);
}
double link_tan(double x, map_arg_tp typ, void *param, double *cov)
{
	return map_invtan(x, typ, param);
}
double link_cloglog(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	return map_invcloglog(x, typ, param);
}
double link_log(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	return map_exp(x, typ, param);
}
double link_logit(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	return map_invlogit(x, typ, param);
}
double link_identity(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	return map_identity(x, typ, param);
}
double link_logoffset(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	Link_param_tp *p;
	double beta, off;

	if (!cov) {
		char *msg;
		GMRFLib_sprintf(&msg, "You need to pass the covariates to the link.model[logoffset] in the inla()-argument 'link.covariates'");
		inla_error_general(msg);
		exit(1);
	}
	if (cov[0] < 0.0) {
		char *msg;
		GMRFLib_sprintf(&msg, "The covariates to link.model[logoffset] must be all >= 0.0. Yours is [%g].", cov[0]);
		inla_error_general(msg);
		exit(1);
	}

	p = (Link_param_tp *) param;
	beta = exp(p->beta_intern[GMRFLib_thread_id][0]);
	off = beta * cov[0];

	switch (typ) {
	case MAP_FORWARD:
		return off + exp(x);
	case MAP_BACKWARD:
		return log(x - off);
	case MAP_DFORWARD:
		return exp(x);
	case MAP_INCREASING:
		return 1.0;
	default:
		abort();
	}
	return NAN;
}
double link_sslogit(double x, map_arg_tp typ, void *param, double *cov)
{
	Link_param_tp *p;
	double sens, spec, a, b;

	p = (Link_param_tp *) param;
	sens = map_probability(p->sensitivity_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	spec = map_probability(p->specificity_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	// sens * pi + (1-spec)*(1-pi) = a * pi + b
	a = sens + spec - 1.0;
	b = 1.0 - spec;

	switch (typ) {
	case MAP_FORWARD:
		return a / (1.0 + exp(-x)) + b;

	case MAP_BACKWARD:
		assert((x - b) / (x - b - a) > 0.0);
		return log((x - b) / (x - b - a));

	case MAP_DFORWARD:
		return a * exp(-x) / SQR(1.0 + exp(-x));

	case MAP_INCREASING:
		return (a > 0 ? 1 : 0);
	default:
		abort();
	}
	return NAN;
}
double link_special2(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	Link_param_tp *p;
	double beta, f;

	if (!cov) {
		char *msg;
		GMRFLib_sprintf(&msg, "You need to pass the covariate to the link.model[special2] in the inla()-argument 'link.covariates'");
		inla_error_general(msg);
		exit(1);
	}
	if (cov[0] <= 0.0 || cov[0] >= 1.0) {
		char *msg;
		GMRFLib_sprintf(&msg, "The covariate to link.model[special2] must be between 0 and 1. Your is [%g].", cov[0]);
		inla_error_general(msg);
		exit(1);
	}

	p = (Link_param_tp *) param;
	beta = p->beta[GMRFLib_thread_id][0];
	f = (1.0 - cov[0] + cov[0] * exp(beta));

	switch (typ) {
	case MAP_FORWARD:
		return exp(x) * f;
	case MAP_BACKWARD:
		return log(x / f);
	case MAP_DFORWARD:
		return exp(x) * f;
	case MAP_INCREASING:
		return 1.0;
	default:
		abort();
	}
	return NAN;

}
double link_test1(double x, map_arg_tp typ, void *param, double *cov)
{
	/*
	 * the link-functions calls the inverse map-function 
	 */
	Link_param_tp *p;
	double beta;

	p = (Link_param_tp *) param;
	beta = p->beta[GMRFLib_thread_id][0];

	return map_exp(x - beta * cov[0], typ, param);
}
double link_special1(double x, map_arg_tp typ, void *param, double *cov)
{
	Link_param_tp *p;
	int i;
	double covariate_contribution, h = 1.0E-4, prec;

	p = (Link_param_tp *) param;
	prec = map_precision(p->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	covariate_contribution = 0.0;
	for (i = 0; i < p->order; i++) {
		covariate_contribution += p->betas[i][GMRFLib_thread_id][0] * cov[i];
	}
	assert(!ISNAN(covariate_contribution));

	switch (typ) {
	case MAP_FORWARD:
		return gsl_cdf_lognormal_Pinv(inla_Phi(x), covariate_contribution - 0.5 / prec, 1.0 / sqrt(prec));
		break;
	case MAP_BACKWARD:
		return gsl_cdf_ugaussian_Pinv(gsl_cdf_lognormal_P(x, covariate_contribution - 0.5 / prec, 1.0 / sqrt(prec)));
		break;
	case MAP_DFORWARD:
		return (gsl_cdf_lognormal_Pinv(inla_Phi(x + h), covariate_contribution - 0.5 / prec, 1.0 / sqrt(prec)) -
			gsl_cdf_lognormal_Pinv(inla_Phi(x - h), covariate_contribution - 0.5 / prec, 1.0 / sqrt(prec))) / (2.0 * h);
		break;
	case MAP_INCREASING:
		return 1.0;
	default:
		abort();
	}
	abort();
	return 0.0;
}
int inla_make_besag2_graph(GMRFLib_graph_tp ** graph_out, GMRFLib_graph_tp * graph)
{
	int i;
	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, graph);
	for (i = 0; i < graph->n; i++) {
		GMRFLib_ged_add(ged, i, i + graph->n);
	}
	GMRFLib_ged_build(graph_out, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
int inla_make_2diid_graph(GMRFLib_graph_tp ** graph, inla_2diid_arg_tp * arg)
{
	int i;
	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < 2 * arg->n; i += 2) {
		GMRFLib_ged_add(ged, i, i + 1);
	}
	GMRFLib_ged_build(graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
int inla_make_2diid_wishart_graph(GMRFLib_graph_tp ** graph, inla_2diid_arg_tp * arg)
{
	int i;
	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < arg->n; i++) {
		GMRFLib_ged_add(ged, i, i + arg->n);
	}
	GMRFLib_ged_build(graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
int inla_make_iid_wishart_graph(GMRFLib_graph_tp ** graph, inla_iid_wishart_arg_tp * arg)
{
	int i, j, k, n = arg->n, dim = arg->dim;

	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < n; i++) {
		GMRFLib_ged_add(ged, i, i);
		for (j = 0; j < dim; j++) {
			for (k = j + 1; k < dim; k++) {
				GMRFLib_ged_add(ged, i + j * n, i + k * n);
			}
		}
	}
	GMRFLib_ged_build(graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
int inla_make_rw2diid_graph(GMRFLib_graph_tp ** graph, GMRFLib_rw2ddef_tp * def)
{
	int i, n;
	GMRFLib_ged_tp *ged = NULL;
	GMRFLib_graph_tp *g;

	n = def->nrow * def->ncol;
	GMRFLib_make_rw2d_graph(&g, def);
	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < n; i++) {
		GMRFLib_ged_add(ged, i, i);
	}
	GMRFLib_ged_append_graph(ged, g);
	for (i = 0; i < n; i++) {
		GMRFLib_ged_add(ged, i, i + n);
	}
	GMRFLib_ged_build(graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
int inla_make_bym_graph(GMRFLib_graph_tp ** new_graph, GMRFLib_graph_tp * graph)
{
	/*
	 * for layout: see Qfunc_bym() 
	 */

	int i;
	int n = graph->n;
	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < n; i++) {
		GMRFLib_ged_add(ged, i, i);
	}
	GMRFLib_ged_append_graph(ged, graph);
	for (i = 0; i < n; i++) {
		GMRFLib_ged_add(ged, i, i + n);
	}
	GMRFLib_ged_build(new_graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
double Qfunc_bym(int i, int j, void *arg)
{
	/*
	 * the first half is the `linear predictor', the second half is the spatial term. x = [eta, z], eta|.. ~ N(z, prec*I), z ~ GMRF(prec) 
	 */

	inla_bym_Qfunc_arg_tp *a = (inla_bym_Qfunc_arg_tp *) arg;
	int n = a->n;
	double prec_iid = map_precision(a->log_prec_iid[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	if (IMAX(i, j) < n) {
		/*
		 * the iid term 
		 */
		return prec_iid;
	}
	if (IMIN(i, j) >= n) {
		/*
		 * the spatial term + I*prec_iid 
		 */
		return (i == j ? prec_iid : 0.0) + Qfunc_besag(i - n, j - n, a->besag_arg);
	} else {
		/*
		 * the cross-term which is -prec_iid 
		 */
		return -prec_iid;
	}

	/*
	 * should not happen 
	 */
	assert(0 == 1);
	return 0.0;
}
double Qfunc_bym2(int i, int j, void *arg)
{
	inla_bym2_Qfunc_arg_tp *a = (inla_bym2_Qfunc_arg_tp *) arg;
	int n = a->n;
	double prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double phi = map_probability(a->logit_phi[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (IMAX(i, j) < n) {
		return prec / (1.0 - phi);
	}
	if (IMIN(i, j) >= n) {
		return (i == j ? phi / (1.0 - phi) : 0.0) + Qfunc_besag(i - n, j - n, a->besag_arg);
	}
	return -sqrt(phi * prec) / (1.0 - phi);
}
double Qfunc_rw2diid(int i, int j, void *arg)
{
	inla_rw2diid_Qfunc_arg_tp *a = (inla_rw2diid_Qfunc_arg_tp *) arg;
	int n = a->n;
	double prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double phi = map_probability(a->logit_phi[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (IMAX(i, j) < n) {
		return prec / (1.0 - phi);
	}
	if (IMIN(i, j) >= n) {
		return (i == j ? phi / (1.0 - phi) : 0.0) + GMRFLib_rw2d(i - n, j - n, a->rw2ddef);
	}
	return -sqrt(phi * prec) / (1.0 - phi);
}
double Qfunc_group(int i, int j, void *arg)
{
	inla_group_def_tp *a = (inla_group_def_tp *) arg;
	ar_def_tp *ardef = NULL;
	double rho = 0, val, fac, ngroup, prec = 0;
	int igroup, irem, jgroup, jrem, n;

	switch (a->type) {
	case G_EXCHANGEABLE:
		// FIX THIS LATER
		rho = map_group_rho(a->group_rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, (void *) &(a->ngroup));
		break;

	case G_AR1:
		rho = map_rho(a->group_rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		break;

	case G_AR:
		ardef = a->ardef;
		break;

	case G_RW1:
	case G_RW2:
	case G_BESAG:
		prec = map_precision(a->group_prec_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		break;

	default:
		inla_error_general("This should not happen.");
		abort();
	}

	n = a->N;					       /* this is the size before group */
	ngroup = a->ngroup;

	igroup = i / n;
	irem = i % n;
	jgroup = j / n;
	jrem = j % n;

	if (igroup == jgroup) {

		switch (a->type) {
		case G_EXCHANGEABLE:
			fac = -((ngroup - 2.0) * rho + 1.0) / ((rho - 1.0) * ((ngroup - 1.0) * rho + 1.0));
			break;

		case G_AR1:
			if (!(a->cyclic) && (igroup == 0 || igroup == ngroup - 1)) {
				fac = 1.0 / (1.0 - SQR(rho));
			} else {
				fac = (1.0 + SQR(rho)) / (1.0 - SQR(rho));
			}
			break;

		case G_AR:
			fac = Qfunc_ar(igroup, jgroup, (void *) ardef);
			break;

		case G_RW1:
		case G_RW2:
			if (a->crwdef) {
				fac = prec * GMRFLib_crw(igroup, jgroup, (void *) (a->crwdef));
			} else {
				fac = prec * GMRFLib_rw(igroup, jgroup, (void *) (a->rwdef));
			}
			break;

		case G_BESAG:
			fac = prec * Qfunc_besag(igroup, jgroup, (void *) (a->besagdef));
			break;

		default:
			inla_error_general("This should not happen.");
			abort();
		}

		val = a->Qfunc(irem, jrem, a->Qfunc_arg) * fac;
	} else {
		switch (a->type) {
		case G_EXCHANGEABLE:
			fac = rho / ((rho - 1.0) * ((ngroup - 1.0) * rho + 1.0));
			break;

		case G_AR1:
			fac = -rho / (1.0 - SQR(rho));
			break;

		case G_AR:
			fac = Qfunc_ar(igroup, jgroup, (void *) ardef);
			break;

		case G_RW1:
		case G_RW2:
			if (a->crwdef) {
				fac = prec * GMRFLib_crw(igroup, jgroup, (void *) (a->crwdef));
			} else {
				fac = prec * GMRFLib_rw(igroup, jgroup, (void *) (a->rwdef));
			}
			break;

		case G_BESAG:
			fac = prec * Qfunc_besag(igroup, jgroup, (void *) (a->besagdef));
			break;

		default:
			inla_error_general("This should not happen.");
			abort();
		}

		val = a->Qfunc(irem, jrem, a->Qfunc_arg) * fac;
	}

	return val;
}
int inla_make_group_graph(GMRFLib_graph_tp ** new_graph, GMRFLib_graph_tp * graph, int ngroup, int type, int cyclic, int order, GMRFLib_graph_tp * group_graph)
{
	int i, j, n = graph->n;
	GMRFLib_ged_tp *ged = NULL;

	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < ngroup; i++) {
		GMRFLib_ged_append_graph(ged, graph);
	}

	switch (type) {
	case G_EXCHANGEABLE:
		assert(cyclic == 0);
		for (i = 0; i < ngroup; i++) {
			for (j = i + 1; j < ngroup; j++) {
				GMRFLib_ged_insert_graph2(ged, graph, i * n, j * n);
			}
		}
		break;

	case G_AR1:
		assert(ngroup >= 2);
		for (i = 0; i < ngroup - 1; i++) {
			GMRFLib_ged_insert_graph2(ged, graph, i * n, (i + 1) * n);
		}
		if (cyclic) {
			GMRFLib_ged_insert_graph2(ged, graph, 0 * n, (ngroup - 1) * n);
		}
		break;

	case G_AR:
		assert(ngroup >= 2);
		for (i = 0; i < ngroup - 1; i++) {
			for (j = 1; j <= order; j++) {
				if (i + j < ngroup) {
					GMRFLib_ged_insert_graph2(ged, graph, i * n, (i + j) * n);
				}
			}
		}
		break;

	case G_RW1:
		assert(ngroup >= 2);
		for (i = 0; i < ngroup - 1; i++) {
			GMRFLib_ged_insert_graph2(ged, graph, i * n, (i + 1) * n);
		}
		if (cyclic) {
			GMRFLib_ged_insert_graph2(ged, graph, 0 * n, (ngroup - 1) * n);
		}
		break;

	case G_RW2:
		assert(ngroup >= 3);
		for (i = 0; i < ngroup - 2; i++) {
			GMRFLib_ged_insert_graph2(ged, graph, i * n, (i + 1) * n);
			GMRFLib_ged_insert_graph2(ged, graph, i * n, (i + 2) * n);
		}
		GMRFLib_ged_insert_graph2(ged, graph, (ngroup - 2) * n, (ngroup - 1) * n);
		if (cyclic) {
			GMRFLib_ged_insert_graph2(ged, graph, 0 * n, (ngroup - 1) * n);
			GMRFLib_ged_insert_graph2(ged, graph, 0 * n, (ngroup - 2) * n);
			GMRFLib_ged_insert_graph2(ged, graph, 1 * n, (ngroup - 1) * n);
		}
		break;

	case G_BESAG:
		assert(group_graph);
		for (i = 0; i < group_graph->n; i++) {
			int jj;
			for (jj = 0; jj < group_graph->nnbs[i]; jj++) {
				j = group_graph->nbs[i][jj];
				if (j > i) {
					GMRFLib_ged_insert_graph2(ged, graph, i * n, j * n);
				}
			}
		}
		break;

	default:
		inla_error_general("This should not happen");
		abort();
	}

	if (0) {
		FILE *fp = fopen("g.dat", "w");
		GMRFLib_print_graph(fp, new_graph[0]);
		fclose(fp);
	}

	assert(GMRFLib_ged_max_node(ged) == n * ngroup - 1);
	GMRFLib_ged_build(new_graph, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
double Qfunc_generic1(int i, int j, void *arg)
{
	inla_generic1_tp *a = (inla_generic1_tp *) arg;
	double beta = map_probability(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	return prec * ((i == j ? 1.0 : 0.0) - (beta / a->max_eigenvalue) * a->tab->Qfunc(i, j, a->tab->Qfunc_arg));
}
double Qfunc_generic2(int i, int j, void *arg)
{
	/*
	 * [u,v], where u = v + noise. h2 = 1/prec_v / (1/prec_v + 1/prev_u). h2 in (0,1). 
	 */

	inla_generic2_tp *a = (inla_generic2_tp *) arg;

	double prec_cmatrix = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL),
	    h2 = map_probability(a->h2_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), prec_unstruct;
	int n = a->n;

	prec_unstruct = h2 / (1.0 - h2) * prec_cmatrix;

	if (i == j) {

		if (i < n) {
			/*
			 * the sum 
			 */
			return prec_unstruct;
		} else {
			/*
			 * cmatrix 
			 */
			return prec_unstruct + prec_cmatrix * a->tab->Qfunc(i - n, i - n, a->tab->Qfunc_arg);
		}
	} else {
		if (IMIN(i, j) >= n) {
			/*
			 * inside the Cmatrix 
			 */
			return prec_cmatrix * a->tab->Qfunc(i - n, j - n, a->tab->Qfunc_arg);
		} else {
			/*
			 * cross term between cmatrix and the sum 
			 */
			return -prec_unstruct;
		}
	}
	assert(0 == 1);
	return 0.0;
}
double Qfunc_generic3(int i, int j, void *arg)
{
	inla_generic3_tp *a = (inla_generic3_tp *) arg;
	double prec_common, prec, val = 0.0;
	int k, same = (i == j);

	for (k = 0; k < a->m; k++) {
		if (same || GMRFLib_is_neighb(i, j, a->g[k])) {
			prec = map_precision(a->log_prec[k][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
			val += prec * a->tab[k]->Qfunc(i, j, a->tab[k]->Qfunc_arg);
		}
	}
	prec_common = map_precision(a->log_prec[GENERIC3_MAXTHETA - 1][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	val *= prec_common;

	return (val);
}
double Qfunc_replicate(int i, int j, void *arg)
{
	int ii, jj;
	inla_replicate_tp *a = (inla_replicate_tp *) arg;

	ii = MOD(i, a->n);
	jj = MOD(j, a->n);

	return a->Qfunc(ii, jj, a->Qfunc_arg);
}
int inla_replicate_graph(GMRFLib_graph_tp ** g, int replicate)
{
	/*
	 * replace the graph G, with on that is replicated REPLICATE times 
	 */
	int i;
	GMRFLib_ged_tp *ged;

	if (!g || !*g || replicate <= 1) {
		return GMRFLib_SUCCESS;
	}
	GMRFLib_ged_init(&ged, NULL);
	for (i = 0; i < replicate; i++) {
		GMRFLib_ged_append_graph(ged, *g);
	}
	GMRFLib_free_graph(*g);
	GMRFLib_ged_build(g, ged);
	GMRFLib_ged_free(ged);

	return GMRFLib_SUCCESS;
}
double Qfunc_z(int i, int j, void *arg)
{
	inla_z_arg_tp *a = (inla_z_arg_tp *) arg;
	double value = 0.0;

	if (i == j || GMRFLib_is_neighb(i, j, a->graph_A)) {
		value += a->Qfunc_A->Qfunc(i, j, a->Qfunc_A->Qfunc_arg);
	}
	if (i == j || GMRFLib_is_neighb(i, j, a->graph_B)) {
		/*
		 * doit like this, as most of the elements in B are zero
		 */
		double q = a->Qfunc_B->Qfunc(i, j, a->Qfunc_B->Qfunc_arg);
		if (q) {
			value += q * map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		}
	}
	return value;
}
double Qfunc_slm(int i, int j, void *arg)
{
	inla_slm_arg_tp *a = (inla_slm_arg_tp *) arg;
	double value = 0.0, prec, rho, rho_std;

	prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rho_std = map_probability(a->logit_rho[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rho = a->rho_min + rho_std * (a->rho_max - a->rho_min);

	if (0) {
		if (i == j && i == 0) {
			printf("log_prec %.12f  logit_rho %.12f\n", a->log_prec[GMRFLib_thread_id][0], a->logit_rho[GMRFLib_thread_id][0]);
		}
	}

	if (i == j || GMRFLib_is_neighb(i, j, a->graph_A1)) {
		value += prec * a->Qfunc_A1->Qfunc(i, j, a->Qfunc_A1->Qfunc_arg);
	}
	if (i == j || GMRFLib_is_neighb(i, j, a->graph_A2)) {
		value += a->Qfunc_A2->Qfunc(i, j, a->Qfunc_A2->Qfunc_arg);
	}
	if (i == j || GMRFLib_is_neighb(i, j, a->graph_B)) {
		value += prec * rho * a->Qfunc_B->Qfunc(i, j, a->Qfunc_B->Qfunc_arg);
	}
	if (i == j || GMRFLib_is_neighb(i, j, a->graph_C)) {
		value += prec * SQR(rho) * a->Qfunc_C->Qfunc(i, j, a->Qfunc_C->Qfunc_arg);
	}

	return value;
}
double Qfunc_rgeneric(int i, int j, void *arg)
{
	inla_rgeneric_tp *a = (inla_rgeneric_tp *) arg;
	int rebuild, ii, debug = 0, id;

	id = omp_get_thread_num() * GMRFLib_MAX_THREADS + GMRFLib_thread_id;
	rebuild = (a->param[id] == NULL || a->Q[GMRFLib_thread_id] == NULL);
	if (!rebuild) {
		for (ii = 0; ii < a->ntheta && !rebuild; ii++) {
			rebuild = (a->param[id][ii] != a->theta[ii][GMRFLib_thread_id][0]);
		}
	}

	if (rebuild) {
		int *ilist = NULL, *jlist = NULL, len;
		double *Qijlist = NULL;
		GMRFLib_graph_tp *graph = NULL;
#pragma omp critical
		{
			if (debug) {
				printf("Rebuild Q-hash for id %d\n", id);
			}
			if (a->Q[id]) {
				GMRFLib_free_tabulate_Qfunc(a->Q[id]);
			}
			if (!(a->param[id])) {
				a->param[id] = Calloc(a->ntheta, double);
			}
			for (ii = 0; ii < a->ntheta; ii++) {
				a->param[id][ii] = a->theta[ii][GMRFLib_thread_id][0];
			}
			WRITE_MSG(a->c2R, a->Id, R_GENERIC_Q);
			WRITE(a->c2R, a->param[id], a->ntheta, double);
			READ(a->R2c, &len, 1, int);
			ilist = Calloc(2 * len, int);
			jlist = ilist + len;
			Qijlist = Calloc(len, double);
			READ(a->R2c, ilist, len, int);
			READ(a->R2c, jlist, len, int);
			READ(a->R2c, Qijlist, len, double);
			GMRFLib_tabulate_Qfunc_from_list(&(a->Q[id]), &graph, len, ilist, jlist, Qijlist, -1, NULL, NULL, NULL);
		}
		GMRFLib_free_graph(graph);
		Free(ilist);				       /* jlist is ok */
		Free(Qijlist);
	}

	return (a->Q[id]->Qfunc(i, j, a->Q[id]->Qfunc_arg));
}
double Qfunc_clinear(int i, int j, void *arg)
{
	inla_clinear_tp *a = (inla_clinear_tp *) arg;
	assert(i == j);
	return (a->precision);
}
double Qfunc_sigm(int i, int j, void *arg)
{
	inla_sigm_tp *a = (inla_sigm_tp *) arg;
	assert(i == j);
	return (a->precision);
}
double mfunc_clinear(int i, void *arg)
{
	inla_clinear_tp *a = (inla_clinear_tp *) arg;
	double beta = map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->beta_arg);

	return beta * a->x[i];
}
double mfunc_sigm(int i, void *arg)
{
	inla_sigm_tp *a = (inla_sigm_tp *) arg;
	double beta, halflife, shape, x, xx;

	beta = a->beta[GMRFLib_thread_id][0];
	halflife = map_precision(a->log_halflife[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	shape = map_precision(a->log_shape[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	x = a->x[i];
	assert(x >= 0.0);

	xx = pow(x / halflife, shape);
	return beta * xx / (1.0 + xx);
}
double mfunc_revsigm(int i, void *arg)
{
	inla_sigm_tp *a = (inla_sigm_tp *) arg;
	double beta, halflife, shape, x, xx;

	beta = a->beta[GMRFLib_thread_id][0];
	halflife = map_precision(a->log_halflife[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	shape = map_precision(a->log_shape[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	x = a->x[i];
	assert(x >= 0.0);

	xx = pow(x / halflife, shape);
	return beta * 1.0 / (1.0 + xx);
}
double Qfunc_mec(int i, int j, void *arg)
{
	inla_mec_tp *a = (inla_mec_tp *) arg;
	double prec_x = map_precision(a->log_prec_x[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double prec_obs = map_precision(a->log_prec_obs[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double beta = map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);
	double *scale = a->scale;

	assert(i == j);
	return (prec_x + prec_obs * scale[i]) / SQR(beta);
}
double mfunc_mec(int i, void *arg)
{
	inla_mec_tp *a = (inla_mec_tp *) arg;
	double prec_x = map_precision(a->log_prec_x[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double prec_obs = map_precision(a->log_prec_obs[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double beta = map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);
	double mean_x = map_identity(a->mean_x[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double *x_obs = a->x_obs;
	double *scale = a->scale;

	return beta * (prec_obs * scale[i] * x_obs[i] + prec_x * mean_x) / (prec_obs * scale[i] + prec_x);
}
double Qfunc_meb(int i, int j, void *arg)
{
	inla_meb_tp *a = (inla_meb_tp *) arg;
	double prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double beta = map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);
	double *scale = a->scale;

	assert(i == j);
	return prec * scale[i] / SQR(beta);
}
double mfunc_meb(int i, void *arg)
{
	inla_meb_tp *a = (inla_meb_tp *) arg;
	double beta = map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);

	return beta * a->x[i];
}
int inla_iid_wishart_nparam(int dim)
{
	/*
	 * return the number of theta parameters 
	 */
	return ((dim * (dim + 1)) / 2);
}
double Qfunc_iid_wishart(int node, int nnode, void *arg)
{
	/*
	 * This function returns the ij'th element of the precision matrix of the dim-dimensional iid. The parameterisation is given in the covariance matrix, so we
	 * need to compute the precision matrix. We store Q to avoid to compute it all the time.
	 */

	inla_iid_wishart_arg_tp *a = (inla_iid_wishart_arg_tp *) arg;
	int i, j, k, n_theta, dim, debug = 0, id;
	double *vec = NULL;
	inla_wishart_hold_tp *hold = NULL;

	dim = a->dim;
	n_theta = inla_iid_wishart_nparam(a->dim);

	if (dim == 1) {
		/*
		 *  Fast return for the IID1D model; no need to do complicate things in this case
		 */
		return map_precision(a->log_prec[0][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	}


	/*
	 * using this prevent us for using '#pragma omp critical' below, so its much quicker 
	 */
	id = omp_get_thread_num() * GMRFLib_MAX_THREADS + GMRFLib_thread_id;

	assert(a->hold);
	hold = a->hold[id];
	if (hold == NULL) {
		a->hold[id] = Calloc(1, inla_wishart_hold_tp);
		a->hold[id]->vec = Calloc(n_theta, double);
		a->hold[id]->vec[0] = GMRFLib_uniform();
		a->hold[id]->Q = gsl_matrix_calloc(a->dim, a->dim);
		hold = a->hold[id];
	}

	vec = Calloc(n_theta, double);
	k = 0;
	for (i = 0; i < dim; i++) {
		vec[k] = map_precision(a->log_prec[i][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		k++;
	}
	for (i = 0; i < n_theta - dim; i++) {
		vec[k] = map_rho(a->rho_intern[i][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		k++;
	}
	assert(k == n_theta);

	if (memcmp((void *) vec, (void *) hold->vec, n_theta * sizeof(double))) {
		inla_iid_wishart_adjust(dim, vec);
		k = 0;
		for (i = 0; i < dim; i++) {
			gsl_matrix_set(hold->Q, i, i, 1.0 / vec[k]);
			k++;
		}
		for (i = 0; i < dim; i++) {
			for (j = i + 1; j < dim; j++) {
				double value = vec[k] / sqrt(vec[i] * vec[j]);
				gsl_matrix_set(hold->Q, i, j, value);
				gsl_matrix_set(hold->Q, j, i, value);
				k++;
			}
		}
		assert(k == n_theta);

		if (debug) {
			for (i = 0; i < n_theta; i++)
				printf("vec[%1d] = %.12f\n", i, vec[i]);
			FIXME("hold->Q");
			GMRFLib_gsl_matrix_fprintf(stdout, hold->Q, " %.12f");
		}

		GMRFLib_gsl_spd_inverse(hold->Q);
		memcpy((void *) hold->vec, (void *) vec, n_theta * sizeof(double));	/* YES! */
	}

	Free(vec);

	return gsl_matrix_get(hold->Q, node / a->n, nnode / a->n);
}
double Qfunc_iid2d(int i, int j, void *arg)
{
	inla_iid2d_arg_tp *a = (inla_iid2d_arg_tp *) arg;
	double prec0, prec1, rho;

	prec0 = map_precision(a->log_prec0[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec1 = map_precision(a->log_prec1[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rho = map_rho(a->rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	if (i == j) {
		if (i < a->n) {
			return prec0 / (1.0 - SQR(rho));
		} else {
			return prec1 / (1.0 - SQR(rho));
		}
	}

	return -rho * sqrt(prec0 * prec1) / (1.0 - SQR(rho));
}
double Qfunc_2diid(int i, int j, void *arg)
{
	inla_2diid_arg_tp *a = (inla_2diid_arg_tp *) arg;
	double prec0, prec1, rho;

	prec0 = map_precision(a->log_prec0[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec1 = map_precision(a->log_prec1[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rho = map_rho(a->rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	if (i == j) {
		if (GSL_IS_EVEN(i)) {
			return prec0 / (1.0 - SQR(rho));
		} else {
			return prec1 / (1.0 - SQR(rho));
		}
	}

	return -rho * sqrt(prec0 * prec1) / (1.0 - SQR(rho));
}
double Qfunc_2diid_wishart(int i, int j, void *arg)
{
	inla_2diid_arg_tp *a = (inla_2diid_arg_tp *) arg;
	double prec0, prec1, rho;

	prec0 = map_precision(a->log_prec0[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec1 = map_precision(a->log_prec1[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rho = map_rho(a->rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	if (i == j) {
		if (i < a->n) {
			return prec0 / (1.0 - SQR(rho));
		} else {
			return prec1 / (1.0 - SQR(rho));
		}
	}

	return -rho * sqrt(prec0 * prec1) / (1.0 - SQR(rho));
}
int inla_make_ar1_graph(GMRFLib_graph_tp ** graph, inla_ar1_arg_tp * arg)
{
	return GMRFLib_make_linear_graph(graph, arg->n, 1, arg->cyclic);
}
int inla_make_ou_graph(GMRFLib_graph_tp ** graph, inla_ou_arg_tp * arg)
{
	return GMRFLib_make_linear_graph(graph, arg->n, 1, 0);
}
double Qfunc_ar1(int i, int j, void *arg)
{
	inla_ar1_arg_tp *a = (inla_ar1_arg_tp *) arg;
	double phi, prec_marginal, prec, val;

	/*
	 * the log_prec is the log precision for the *marginal*; so we need to compute the log_prec for the innovation or conditional noise. 
	 */
	phi = map_phi(a->phi_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec_marginal = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec = prec_marginal / (1.0 - SQR(phi));

	if (a->cyclic) {
		if (i == j) {
			val = prec * (1.0 + SQR(phi));
		} else {
			val = -prec * phi;
		}
	} else {
		if (i != j) {
			val = -prec * phi;
		} else {
			if (i == 0 || i == a->n - 1) {
				val = prec;
			} else {
				val = prec * (1.0 + SQR(phi));
			}
		}
	}

	return val;
}
double Qfunc_ou(int i, int j, void *arg)
{
	inla_ou_arg_tp *a = (inla_ou_arg_tp *) arg;
	double phi, prec, w, v, delta;

	phi = map_exp(a->phi_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	if (i != j) {
		int ii = IMAX(i, j);
		delta = a->locations[ii] - a->locations[ii - 1];
		w = 1.0 / (1.0 - exp(-2.0 * phi * delta));
		v = exp(-phi * delta);

		return -prec * w * v;
	} else {
		if (i == 0) {
			delta = a->locations[i + 1] - a->locations[i];
			w = 1.0 / (1.0 - exp(-2.0 * phi * delta));
			v = exp(-phi * delta);

			return prec * (1.0 + w * SQR(v));
		} else if (i == a->n - 1) {
			delta = a->locations[i] - a->locations[i - 1];
			w = 1.0 / (1.0 - exp(-2.0 * phi * delta));

			return prec * w;
		} else {
			delta = a->locations[i + 1] - a->locations[i];
			w = 1.0 / (1.0 - exp(-2.0 * phi * delta));
			v = exp(-phi * delta);

			double ddelta = a->locations[i] - a->locations[i - 1];
			double ww = 1.0 / (1.0 - exp(-2.0 * phi * ddelta));

			return prec * (ww + w * SQR(v));
		}
	}
	abort();
	return 0.0;
}
double priorfunc_pc_spde_ga(double *x, double *parameters)
{
	double theta1 = x[0], theta2 = x[1], *par = parameters, ldens, lam1, lam2;
	int debug = 0;

	lam1 = -par[0] * log(par[1]);
	lam2 = -log(par[3]) / par[2];
	ldens = (log(lam1) - theta1 - lam1 * exp(-theta1)) + (log(lam2) + theta2 - lam2 * exp(theta2));

	if (debug) {
		fprintf(stderr, "pc_spde_ga: x = %g %g\n", x[0], x[1]);
		fprintf(stderr, "            param = %g %g %g %g\n", par[0], par[1], par[2], par[3]);
		fprintf(stderr, "            lam1 = %g  lam2 = %g  ldens = %g\n", lam1, lam2, ldens);
	}

	return ldens;
}
double priorfunc_pc_dof(double *x, double *parameters)
{
#define NP 5
	int k, debug = 0;
	double u = parameters[0], alpha = parameters[1], lambda, dof, val, deriv;
	double wf[] = { 1.0 / 12.0, -2.0 / 3.0, 0.0, 2.0 / 3.0, -1.0 / 12.0 };
	double step, dofs[NP], f[NP];

	dof = map_dof(*x, MAP_FORWARD, NULL);
	lambda = -log(alpha) / inla_pcp_dof_d(u);
	// be somewhat careful evaluating the derivative
	step = sqrt(dof) * 1e-3;			       /* step-size is found empirically */
	if (dof - 2.0 * step < 2.003) {			       /* if we're to close to the lower limit */
		step = (dof - 2.003) / 2.0;
	}

	dofs[0] = dof - 2.0 * step;
	dofs[1] = dof - step;
	dofs[2] = dof;
	dofs[3] = dof + step;
	dofs[4] = dof + 2.0 * step;
	for (k = 0; k < NP; k++) {
		f[k] = inla_pcp_dof_d(dofs[k]);
	}
	deriv = (wf[0] * f[0] + wf[1] * f[1] + wf[2] * f[2] + wf[3] * f[3] + wf[4] * f[4]) / step;
	val = log(lambda) - lambda * f[2] + log(ABS(deriv)) + log(ABS(map_dof(*x, MAP_DFORWARD, NULL)));

	if (debug) {
		P(dof);
		P(f[2]);
		P(deriv);
		P(lambda);
		P(val);
	}

	return val;
#undef NP
}
double priorfunc_pc_prec(double *x, double *parameters)
{
	double u = parameters[0], alpha = parameters[1], theta, val, xx2;

	theta = -log(alpha) / u;
	xx2 = (*x) / 2.0;
	val = log(theta / 2.0) - theta * exp(-xx2) - xx2;

	return val;
}
double priorfunc_pc_rho0(double *x, double *parameters)
{
	// alpha = Prob(rho > u)
	int debug = 0;
	double u = parameters[0], alpha = parameters[1];
	double rho = map_rho(*x, MAP_FORWARD, NULL);
	double ldens, lambda, ljac, val, mu;

	mu = sqrt(-log(1.0 - SQR(rho)));
	lambda = -log(alpha) / sqrt(-log(1.0 - SQR(u)));
	ldens = log(lambda) - lambda * mu + log(ABS(rho) / (1.0 - SQR(rho))) - log(mu);
	ljac = log(ABS(map_rho(*x, MAP_DFORWARD, NULL)));

	val = ldens + ljac;

	if (debug) {
		fprintf(stderr, "priorfunc_pc_rho0: mu %g lambda %g ldens %g ljac %g\n", mu, lambda, ldens, ljac);
		fprintf(stderr, "priorfunc_pc_rho0: theta %g val %g\n", *x, val);
	}

	return val;
}
double priorfunc_pc_rho1(double *x, double *parameters)
{
	// alpha = Prob(rho > u)
	int debug = 0;
	double u = parameters[0], alpha = parameters[1];
	double lambda, rho, ljac, ldens, val, mu;

	// solve for lambda
#define Fsolve(_lam) (((1.0 - exp(-(_lam)*sqrt(1.0-u)))/(1.0-exp(-(_lam)*M_SQRT2))) - alpha)

	int count = 0, count_max = 10000;
	double lambda_initial = -1.0, lambda_step = 0.01, h = GMRFLib_eps(1. / 3.), eps_lambda = GMRFLib_eps(0.5), df;

	if (!(u > -1.0 && u < 1.0 && alpha > sqrt((1.0 - u) / 2.0) && alpha < 1.0)) {
		char *msg;
		GMRFLib_sprintf(&msg, "Wrong rho1 prior-parameters. We must have alpha > sqrt((1-u)/2); see the documentation.");
		inla_error_general(msg);
		exit(1);
	}

	lambda = 1.0;
	if (Fsolve(lambda) > 0.0) {
		while (Fsolve(lambda) > 0.0) {
			lambda += lambda_step;
			assert(count++ < count_max);
		}
	} else {
		while (Fsolve(lambda) < 0.0 && lambda >= eps_lambda) {
			lambda -= lambda_step;
			assert(count++ < count_max);
		}
	}

	if (debug) {
		printf("priorfunc_pc_rho1: u=%g alpha=%g  initial value for lambda=%g\n", u, alpha, lambda);
	}

	count = 0;
	while (ABS(lambda - lambda_initial) > eps_lambda) {
		lambda_initial = lambda;
		df = (Fsolve(lambda_initial + h) - Fsolve(lambda_initial - h)) / (2.0 * h);
		lambda = lambda_initial - Fsolve(lambda) / df;
		if (debug) {
			printf("priorfunc_pc_rho1: iteration=%d lambda=%g\n", count, lambda);
		}
		assert(count++ < count_max);
	}
	if (debug)
		printf("priorfunc_pc_rho1: function value %g\n", Fsolve(lambda));

#undef Fsolve
	rho = map_rho(*x, MAP_FORWARD, NULL);
	mu = sqrt(1.0 - rho);
	ldens = log(lambda) - lambda * mu - log(1.0 - exp(-lambda * M_SQRT2)) - log(2.0 * mu);
	ljac = log(ABS(map_rho(*x, MAP_DFORWARD, NULL)));
	val = ldens + ljac;

	return (val);
}
double priorfunc_jeffreys_df_student_t(double *x, double *parameters)
{
	double df = map_dof(x[0], MAP_FORWARD, NULL);
	double value, log_jacobian;

	if (1) {
#define DIGAMMA(xx) gsl_sf_psi(xx)
#define TRIGAMMA(xx) gsl_sf_psi_1(xx)

		value = 0.5 * log(df / (df + 3.0)) + 0.5 * log(TRIGAMMA(df / 2.0) - TRIGAMMA((df + 1.0) / 2.0) - 2.0 * (df + 3.0) / (df * SQR(df + 1.0)))
		    - log(0.7715233664);		       /* normalising constant: computed in R from 2 to infinity */

		log_jacobian = x[0];
		return value + log_jacobian;
#undef DIGAMMA
#undef TRIGAMMA
	} else {
		value = log(1.0 / df);
		log_jacobian = x[0];
		return value + log_jacobian;
	}
}
double priorfunc_bymjoint(double *logprec_besag, double *p_besag, double *logprec_iid, double *p_iid)
{
	/*
	 * This is the Jon Wakefield prior. Notation: U Spatial, V unstruct.  Conceptually, there is a prior for total variance (U + V) and a prior for the
	 * proportion of the total that is spatial. The spatial variance is approximated as the mean of the marginal spatial variances in an ICAR model.  This mean
	 * is a function of the (known) neighborhood structure and the conditional variance parameter.  Thus, the joint distribution is expressed of the conditional
	 * spatial variance parameter and the non-spatial parameter. The value returned, is the log of the joint for the log-precisions, (-\log Conditional.Var(U),
	 * -\log Var(V))
	 */

	double a, b, c, d, var_u, var_v, val, mean_ievalue;

	a = p_besag[0];					       /* Parameters for the IGamma(a,b) */
	b = p_besag[1];
	mean_ievalue = p_besag[2];			       /* mean(1/eigenvalues) for the reference precision matrix */
	c = p_iid[0];					       /* Parameters for the Beta(c,d) */
	d = p_iid[1];

	var_u = 1.0 / exp(*logprec_besag);		       /* var_u = Conditional.Var(U) i.e. Cond.var_u_i = var_u/n_i */
	var_v = 1.0 / exp(*logprec_iid);

	/*
	 * the log prior for Conditonal.Var(U) and Var(V) 
	 */
	val = a * log(b) + gsl_sf_lngamma(c + d) - gsl_sf_lngamma(a) - gsl_sf_lngamma(c) - gsl_sf_lngamma(d)
	    - (a + c + d) * log((var_u * mean_ievalue) + var_v) + (c - 1.0) * log(var_u * mean_ievalue)
	    + (d - 1.0) * log(var_v) - b / ((var_u * mean_ievalue) + var_v) + log(mean_ievalue);

	/*
	 * the Jacobian for converting to (-\log Conditional.Var(U), -\log Var(V)) 
	 */
	val += log(var_u) + log(var_v);

	return val;
}
double priorfunc_betacorrelation(double *x, double *parameters)
{
	/*
	 * The prior for the correlation coefficient \rho is Beta(a,b), scaled so that it is defined on (-1,1)
	 * The function returns the log prior for \rho.intern = log((1 +\rho)/(1-\rho))
	 */
	double p = exp(*x) / (1.0 + exp(*x)), a = parameters[0], b = parameters[1];
	return log(gsl_ran_beta_pdf(p, a, b)) + 2.0 * (*x) - 2.0 * log(1.0 + exp(*x));
}
double priorfunc_logitbeta(double *x, double *parameters)
{
	/*
	 * The prior for the the logit of a Beta(a,b), logit(p) = log(p/(1-p))
	 */
	double p = exp(*x) / (1.0 + exp(*x)), a = parameters[0], b = parameters[1];
	return log(gsl_ran_beta_pdf(p, a, b)) + (*x) - 2.0 * log(1.0 + exp(*x));
}

double priorfunc_logflat(double *x, double *parameters)
{
	return exp(*x);
}
double priorfunc_logiflat(double *x, double *parameters)
{
	return exp(-*x);
}
double priorfunc_flat(double *x, double *parameters)
{
	return 0.0;
}
double priorfunc_minuslogsqrtruncnormal(double *x, double *parameters)
{
	/*
	 * Requested feature from Sophie Ancelet.
	 * 
	 * This is the prior for \sigma ~ TrucNormal(mean, 1/prior_prec), where \sigma > 0, and the internal parameter is \log\tau = \log(1/\sigma^2) = -\log
	 * \sigma^2, which explans the name.
	 */
	double sd = exp(-0.5 * (*x)), val;

	val = priorfunc_normal(&sd, parameters) - log(gsl_cdf_gaussian_Q(-parameters[0], 1.0 / sqrt(parameters[1]))) +
	    // log(Jacobian)
	    log(ABS(-0.5 * sd));

	return val;
}
double priorfunc_pc_ar(double *x, double *parameters)
{
	int i, p;
	double lambda, *b, *gamma, *pacf, ldens, logjac;

	p = (int) parameters[1];
	lambda = parameters[0];
	b = Calloc(p, double);
	gamma = Calloc(p, double);
	pacf = Calloc(p, double);

	for (i = 0, logjac = 0.0; i < p; i++) {
		b[i] = 0.5;
		// x is internal and this gives us the pacf. 
		pacf[i] = map_phi(x[i], MAP_FORWARD, NULL);
		// but the pc-simplex prior is given in terms of 'gamma'
		gamma[i] = -log(1.0 - SQR(pacf[i]));
		// hence we need two jacobians, one for x->pacf and one for pacf->gamma. recall that we have a singularity for x[i]=0
		double xtmp = (ISZERO(pacf[i]) ? DBL_EPSILON : pacf[i]);
		logjac += log(ABS(map_phi(x[i], MAP_DFORWARD, NULL))) + log(ABS(xtmp / (1.0 - SQR(pacf[i]))));
	}
	ldens = inla_pc_simplex_d(gamma, b, p, lambda) + logjac;

	Free(b);
	Free(gamma);
	Free(pacf);

	return (ldens);
}
double priorfunc_beta(double *x, double *parameters)
{
	double xx = *x, a = parameters[0], b = parameters[1];

	return log(gsl_ran_beta_pdf(xx, a, b));
}
double priorfunc_loggamma(double *x, double *parameters)
{
	/*
	 * return log(loggamma(x,a,b)). NOTE: if y ~ gamma(a,b), then log(y) ~ loggamma(a,b). 
	 */
	double val = exp(*x);

	return priorfunc_gamma(&val, parameters) + (*x);
}
double priorfunc_gamma(double *x, double *parameters)
{
	/*
	 * return log(gamma(x, a, b))
	 *
	 * parametes (a,b) are such that E(x) = a/b and Var(x) = a/b^2
	 */
	double a = parameters[0], b = 1.0 / parameters[1];

	if (0) {
		/*
		 * this crash of'course for extreme values 
		 */
		return log(gsl_ran_gamma_pdf(*x, a, b));
	} else {
		/*
		 * while this is ok. (code copied from GSL...) 
		 */
		if (*x < 0.0) {
			assert(*x >= 0.0);
		} else if (*x == 0.0) {
			if (a == 1.0) {
				return 1.0 / b;
			} else {
				assert(0 == 1);
			}
		} else if (a == 1.0) {
			return -(*x) / b - log(b);
		} else {
			return (a - 1.0) * log((*x) / b) - (*x) / b - gsl_sf_lngamma(a) - log(b);
		}
	}
	assert(0 == 1);
	return 0.0;
}
double priorfunc_gaussian(double *x, double *parameters)
{
	return priorfunc_normal(x, parameters);
}
double priorfunc_normal(double *x, double *parameters)
{
	/*
	 * return log(normal(x, mean, precision)) 
	 */
	double mean = parameters[0], sigma = sqrt(1.0 / parameters[1]);

	return log(gsl_ran_gaussian_pdf((*x) - mean, sigma));
}
double priorfunc_mvnorm(double *x, double *parameters)
{
	/*
	 * this is the multivariate normal 
	 */
	int n = (int) parameters[0], i, j;

	if (n == 0) {
		return 0.0;
	}

	double *mean, *Q, *chol, *xx, q = 0.0, logdet = 0.0;

	mean = &(parameters[1]);
	Q = &(parameters[1 + n]);
	chol = NULL;
	xx = Calloc(n, double);

	if (0) {
		for (i = 0; i < n; i++) {
			for (j = 0; j < n; j++)
				printf("%g ", Q[i + j * n]);
			printf("\n");
		}
		for (i = 0; i < n; i++)
			printf("%g\n", mean[i]);
	}

	GMRFLib_comp_chol_general(&chol, Q, n, &logdet, 0);
	for (i = 0; i < n; i++) {
		xx[i] = x[i] - mean[i];
	}

	/*
	 * q = xx^T * Q * xx. I dont have any easy function for matrix vector except the messy BLAS-FORTRAN-INTERFACE.... so I just do this manually now. FIXME
	 * later. 
	 */
	q = 0.0;
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			q += xx[i] * Q[i + n * j] * xx[j];
		}
	}

	Free(xx);
	Free(chol);

	return (-n / 2.0 * log(2 * M_PI) + 0.5 * logdet - 0.5 * q);
}
int inla_iid_wishart_adjust(int dim, double *theta)
{
	/*
	 * adjust rho's in theta with factor f until the matrix is SPD. 
	 */
#define IDX(_i, _j) ((_i) + (_j)*(dim))

	int i, j, k, ok = 0, debug = 0;
	int n_theta = inla_iid_wishart_nparam(dim);
	double f = 0.95, *S = NULL, *chol = NULL;

	S = Calloc(ISQR(dim), double);
	while (!ok) {
		k = 0;
		for (i = 0; i < dim; i++) {
			S[IDX(i, i)] = 1.0 / theta[k];
			k++;
		}
		for (i = 0; i < dim; i++) {
			for (j = i + 1; j < dim; j++) {
				S[IDX(i, j)] = S[IDX(j, i)] = theta[k] / sqrt(theta[i] * theta[j]);
				k++;
			}
		}
		assert(k == n_theta);

		if (debug) {
			FIXME("in the adjust");
			printf("\n");
			for (i = 0; i < dim; i++) {
				for (j = 0; j < dim; j++) {
					printf(" %.12f", S[IDX(i, j)]);
				}
				printf("\n");
			}
			printf("\n");
		}

		if (GMRFLib_comp_chol_general(&chol, S, dim, NULL, !GMRFLib_SUCCESS) != GMRFLib_SUCCESS) {
			/*
			 * only adjust the rho's 
			 */
			if (debug) {
				printf("matrix is not spd, adjust with factor %f\n", f);
			}
			for (i = dim; i < n_theta; i++) {
				theta[i] *= f;
			}
		} else {
			ok = 1;
		}
	}
	Free(S);
	Free(chol);

#undef IDX
	return (ok ? GMRFLib_SUCCESS : !GMRFLib_SUCCESS);
}
double priorfunc_wishart1d(double *x, double *parameters)
{
	// its the same and quicker

	// return priorfunc_wishart_generic(1, x, parameters);

	double p[2];
	p[0] = parameters[0] / 2.0;
	p[1] = parameters[1] / 2.0;

	return priorfunc_gamma(x, p);
}
double priorfunc_wishart2d(double *x, double *parameters)
{
	return priorfunc_wishart_generic(2, x, parameters);
}
double priorfunc_wishart3d(double *x, double *parameters)
{
	return priorfunc_wishart_generic(3, x, parameters);
}
double priorfunc_wishart4d(double *x, double *parameters)
{
	return priorfunc_wishart_generic(4, x, parameters);
}
double priorfunc_wishart5d(double *x, double *parameters)
{
	return priorfunc_wishart_generic(5, x, parameters);
}
double priorfunc_wishart_generic(int idim, double *x, double *parameters)
{
	/*
	 * 
	 * Q ~ Wishart(r, R^{-1} )
	 * 
	 * input is x = [ tau0, tau1, tau2, rho01, rho02, rho12 ] which parameterise Q.
	 * 
	 * prior parameters are p = [ r, R00, R11, R22, R12, R13, R23 ]
	 * 
	 * output is the logdensity for x!!!! (BE AWARE)
	 */
	gsl_matrix *R = NULL, *Q = NULL;
	double r, val;
	int debug = 0, fail;
	size_t i, ii, j, k, dim = (size_t) idim;

	size_t n_x = (size_t) inla_iid_wishart_nparam(idim);
	size_t n_param = n_x + 1;

	if (debug) {
		for (i = 0; i < n_param; i++) {
			printf("parameters[%d] = %g\n", (int) i, parameters[i]);
		}
		for (i = 0; i < n_x; i++) {
			printf("x[%d] = %g\n", (int) i, x[i]);
		}
	}

	r = parameters[0];
	R = gsl_matrix_calloc(dim, dim);
	Q = gsl_matrix_calloc(dim, dim);

	fail = inla_iid_wishart_adjust(idim, x);

	/*
	 * offset of 1, since parameters[0] = r
	 */
	k = 1;
	for (i = 0; i < dim; i++) {
		gsl_matrix_set(R, i, i, parameters[k]);
		k++;
	}
	for (i = 0; i < dim; i++) {
		for (j = i + 1; j < dim; j++) {
			gsl_matrix_set(R, i, j, parameters[k]);
			gsl_matrix_set(R, j, i, parameters[k]);
			k++;
		}
	}
	assert(k == n_param);

#define COMPUTE_Q(Q_)							\
	if (1) {							\
		k = 0;							\
		for(i = 0; i<dim; i++) {				\
			gsl_matrix_set(Q_, i, i, 1/x[k]);		\
			k++;						\
		}							\
		for(i=0; i<dim; i++) {					\
			for(j=i+1; j < dim; j++) {			\
				double value = x[k] / sqrt(x[i] * x[j]); \
				gsl_matrix_set(Q_, i, j, value);	\
				gsl_matrix_set(Q_, j, i, value);	\
				k++;					\
			}						\
		}							\
		assert(k == n_x);					\
		if (debug) printf("Covmatrix:\n");			\
		if (debug) GMRFLib_gsl_matrix_fprintf(stdout, Q_, NULL); \
		if (debug) printf("Precision:\n");			\
		if (debug) GMRFLib_gsl_matrix_fprintf(stdout, Q_, NULL); \
		GMRFLib_gsl_spd_inverse(Q_);				\
	}


	COMPUTE_Q(Q);
	val = GMRFLib_Wishart_logdens(Q, r, R) + (fail ? PENALTY : 0.0);

	/*
	 * tau1 and tau1 are the *MARGINAL* precisions, which it should be.  The jacobian is computed like this:
	 * 
	 * with(LinearAlgebra); with(Student[VectorCalculus]); S := matrix(3,3, [ 1/tau0, rho01/sqrt(tau0*tau1), rho02/sqrt(tau0*tau2), rho01/sqrt(tau0*tau1),
	 * 1/tau1, rho12/sqrt(tau1*tau2), rho02/sqrt(tau0*tau2), rho12/sqrt(tau1*tau2), 1/tau2]); Q := inverse(S); simplify(Determinant(Jacobian([Q[1,1], Q[2,2],
	 * Q[3,3], Q[1,2], Q[1,3],Q[2,3]], [tau0,tau1,tau2,rho01,rho02,rho12])));
	 * 
	 * this gives a very long answer of'course; so we have to do this numerically 
	 */
	gsl_matrix *QQ = NULL, *J = NULL;
	double f, save;

	/*
	 * for the numerical derivatives: compute the `population' variance: Det(Sigma), and set f = (Det(Sigma))^1/dim. 
	 */
	f = 1.0e-6 * pow(exp(-GMRFLib_gsl_spd_logdet(Q)), 1.0 / (double) idim);	/* Yes, its a minus... */
	QQ = GMRFLib_gsl_duplicate_matrix(Q);
	J = gsl_matrix_calloc(n_x, n_x);

	/*
	 * the precision terms *can* get negative since we're using a central estimate, so we need to check the step-size 
	 */
	for (ii = 0; ii < dim; ii++) {
		f = DMIN(f, 0.5 * x[ii]);
	}

	for (ii = 0; ii < n_x; ii++) {
		save = x[ii];

		x[ii] += f;
		COMPUTE_Q(Q);
		x[ii] = save;

		x[ii] -= f;
		COMPUTE_Q(QQ);
		x[ii] = save;

		k = 0;
		for (i = 0; i < dim; i++) {
			gsl_matrix_set(J, ii, k, (gsl_matrix_get(Q, k, k) - gsl_matrix_get(QQ, k, k)) / (2.0 * f));
			assert(k == i);
			k++;
		}
		for (i = 0; i < dim; i++) {
			for (j = i + 1; j < dim; j++) {
				gsl_matrix_set(J, ii, k, (gsl_matrix_get(Q, i, j) - gsl_matrix_get(QQ, i, j)) / (2.0 * f));
				k++;
			}
		}
	}
	assert(k == n_x);

	gsl_permutation *p;
	int signum;
	double logdet;

	p = gsl_permutation_alloc(n_x);
	gsl_linalg_LU_decomp(J, p, &signum);
	logdet = gsl_linalg_LU_lndet(J);		       /* log(abs|J|) */

	if (debug) {
		P(logdet);
	}

	gsl_matrix_free(R);
	gsl_matrix_free(Q);
	gsl_matrix_free(QQ);
	gsl_matrix_free(J);
	gsl_permutation_free(p);

	val += logdet;

#undef COMPUTE_Q

	return val;
}
double Qfunc_besag(int i, int j, void *arg)
{
	inla_besag_Qfunc_arg_tp *a;
	double prec;

	a = (inla_besag_Qfunc_arg_tp *) arg;
	if (a->log_prec) {
		prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	} else {
		prec = 1.0;
	}
	prec *= (a->prec_scale ? a->prec_scale[0] : 1.0);
	return prec * (i == j ? a->graph->nnbs[i] : -1.0);
}
double Qfunc_besag2(int i, int j, void *arg)
{
	inla_besag2_Qfunc_arg_tp *aa;
	double prec, a;

	aa = (inla_besag2_Qfunc_arg_tp *) arg;

	if (aa->log_prec) {
		prec = map_precision(aa->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	} else {
		prec = 1.0;
	}
	prec *= (aa->prec_scale ? aa->prec_scale[0] : 1.0);

	if (aa->log_a) {
		a = map_exp(aa->log_a[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	} else {
		a = 1.0;
	}

	if (IMAX(i, j) < aa->graph->n) {
		if (i == j) {
			return (prec * aa->graph->nnbs[i] + aa->precision) / SQR(a);
		} else {
			return -prec / SQR(a);
		}
	} else if (IMIN(i, j) >= aa->graph->n) {
		return aa->precision * SQR(a);
	} else {
		return -aa->precision;
	}
}
double Qfunc_besagproper(int i, int j, void *arg)
{
	inla_besag_proper_Qfunc_arg_tp *a;
	double prec;

	a = (inla_besag_proper_Qfunc_arg_tp *) arg;
	prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (i == j) {
		double diag = map_exp(a->log_diag[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
		return prec * (diag + a->graph->nnbs[i]);
	} else {
		return -prec;
	}
}
double Qfunc_besagproper2(int i, int j, void *arg)
{
	inla_besag_proper2_Qfunc_arg_tp *a;
	double prec;
	double lambda;

	a = (inla_besag_proper2_Qfunc_arg_tp *) arg;
	prec = map_precision(a->log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	lambda = map_probability(a->logit_lambda[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (i == j) {
		return prec * ((1.0 - lambda) + lambda * a->graph->nnbs[i]);
	} else {
		return -prec * lambda;
	}
}
int inla_read_data_all(double **x, int *n, const char *filename)
{
	if (!filename) {
		/*
		 * useful for ini-files with no weight file. (backward compatability...)
		 */
		*n = 0;
		*x = NULL;
		return GMRFLib_SUCCESS;
	}

	int count = 0, err, len = 1000;
	double *c = Calloc(len, double);
	GMRFLib_io_tp *io = NULL;

	if (GMRFLib_is_fmesher_file(filename, (long int) 0, -1) == GMRFLib_SUCCESS) {
		/*
		 * This is the binary-file interface 
		 */
		GMRFLib_matrix_tp *M = GMRFLib_read_fmesher_file(filename, (long int) 0, -1);
		assert(M->elems == M->nrow * M->ncol);	       /* no sparse matrix! */

		*n = M->nrow * M->ncol;
		*x = Calloc(*n, double);

		int i, j, k;
		for (i = k = 0; i < M->nrow; i++) {
			for (j = 0; j < M->ncol; j++) {
				(*x)[k++] = M->A[i + j * M->nrow];
			}
		}
		GMRFLib_matrix_free(M);

		return INLA_OK;
	} else {
		GMRFLib_EWRAP0(GMRFLib_io_open(&io, filename, "r"));
		{
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();

			while (1) {
				err = GMRFLib_io_read_next(io, &c[count], "%lf");
				if (err == GMRFLib_SUCCESS) {
					count++;
					if (count >= len) {
						len += 1000;
						c = Realloc(c, len, double);
					}
				} else {
					break;
				}
			}
			GMRFLib_set_error_handler(old_handler);
		}
		GMRFLib_EWRAP0(GMRFLib_io_close(io));

		*n = count;
		if (count == 0) {
			Free(c);
			*x = NULL;
		} else {
			*x = c;
		}

		return INLA_OK;
	}
}
int inla_read_data_likelihood(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * read data from file 
	 */
	double *x = NULL, *a[128];
	int n, na, i, j, ii, idiv = 0, k;
	Data_section_tp *ds = &(mb->data_sections[mb->nds - 1]);

	/*
	 * first read all entries in the file 
	 */
	inla_read_data_all(&x, &n, ds->data_file.name);
	if (mb->verbose) {
		printf("\t\tread n=[%1d] entries from file=[%s]\n", n, ds->data_file.name);
	}
	switch (ds->data_id) {
	case L_GAUSSIAN:
		idiv = 3;
		a[0] = ds->data_observations.weight_gaussian = Calloc(mb->predictor_ndata, double);
		break;

	case L_SIMPLEX:
		idiv = 3;
		a[0] = ds->data_observations.weight_simplex = Calloc(mb->predictor_ndata, double);
		break;

	case L_IID_GAMMA:
		idiv = 3;
		a[0] = ds->data_observations.iid_gamma_weight = Calloc(mb->predictor_ndata, double);
		break;

	case L_IID_LOGITBETA:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_LOGGAMMA_FRAILTY:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_LOGISTIC:
		idiv = 3;
		a[0] = ds->data_observations.weight_logistic = Calloc(mb->predictor_ndata, double);
		break;

	case L_SKEWNORMAL:
		idiv = 3;
		a[0] = ds->data_observations.weight_skew_normal = Calloc(mb->predictor_ndata, double);
		break;

	case L_SKEWNORMAL2:
		idiv = 3;
		a[0] = ds->data_observations.weight_skew_normal2 = Calloc(mb->predictor_ndata, double);
		break;

	case L_GEV:
		idiv = 3;
		a[0] = ds->data_observations.weight_gev = Calloc(mb->predictor_ndata, double);
		break;

	case L_LAPLACE:
		idiv = 3;
		a[0] = ds->data_observations.weight_laplace = Calloc(mb->predictor_ndata, double);
		break;

	case L_T:
		idiv = 3;
		a[0] = ds->data_observations.weight_t = Calloc(mb->predictor_ndata, double);
		break;

	case L_TSTRATA:
		idiv = 4;
		a[0] = ds->data_observations.weight_tstrata = Calloc(mb->predictor_ndata, double);
		a[1] = ds->data_observations.strata_tstrata = Calloc(mb->predictor_ndata, double);
		break;

	case L_POISSON:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_GPOISSON:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDPOISSON0:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDPOISSON1:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDPOISSON2:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_BINOMIAL:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_TEST_BINOMIAL_1:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_CBINOMIAL:
		idiv = 4;
		a[0] = ds->data_observations.cbinomial_k = Calloc(mb->predictor_ndata, double);
		a[1] = ds->data_observations.cbinomial_n = Calloc(mb->predictor_ndata, double);
		break;

	case L_GAMMA:
		idiv = 3;
		a[0] = ds->data_observations.gamma_weight = Calloc(mb->predictor_ndata, double);
		break;

	case L_GAMMACOUNT:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_BETA:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_BETABINOMIAL:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBINOMIAL0:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBINOMIAL1:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBINOMIAL2:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZERO_N_INFLATEDBINOMIAL2:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBETABINOMIAL0:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBETABINOMIAL1:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDBETABINOMIAL2:
		idiv = 3;
		a[0] = ds->data_observations.nb = Calloc(mb->predictor_ndata, double);
		break;

	case L_NBINOMIAL:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDNBINOMIAL0:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDNBINOMIAL1:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDNBINOMIAL2:
		idiv = 3;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		break;

	case L_ZEROINFLATEDNBINOMIAL1STRATA2:
	case L_ZEROINFLATEDNBINOMIAL1STRATA3:
		idiv = 4;
		a[0] = ds->data_observations.E = Calloc(mb->predictor_ndata, double);
		a[1] = ds->data_observations.strata = Calloc(mb->predictor_ndata, double);
		break;

	case L_STOCHVOL:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_STOCHVOL_T:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_STOCHVOL_NIG:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_LOGPERIODOGRAM:
		idiv = 2;
		a[0] = NULL;
		break;

	case L_EXPONENTIAL:
	case L_WEIBULL:
	case L_WEIBULL_CURE:
	case L_LOGLOGISTIC:
	case L_LOGNORMAL:
		idiv = 6;
		a[0] = ds->data_observations.event = Calloc(mb->predictor_ndata, double);	/* the failure code */
		a[1] = ds->data_observations.truncation = Calloc(mb->predictor_ndata, double);
		a[2] = ds->data_observations.lower = Calloc(mb->predictor_ndata, double);
		a[3] = ds->data_observations.upper = Calloc(mb->predictor_ndata, double);
		break;

	case L_CIRCULAR_NORMAL:
		idiv = 3;
		a[0] = ds->data_observations.weight_circular_normal = Calloc(mb->predictor_ndata, double);
		break;

	case L_WRAPPED_CAUCHY:
		idiv = 3;
		a[0] = ds->data_observations.weight_wrapped_cauchy = Calloc(mb->predictor_ndata, double);
		break;

	default:
		assert(0 == 1);
	}
	na = idiv - 2;
	if (!inla_divisible(n, idiv)) {
		inla_error_file_numelm(__GMRFLib_FuncName, ds->data_file.name, n, idiv);
	}
	ds->data_observations.ndata = n / idiv;
	ds->data_observations.y = Calloc(mb->predictor_ndata, double);
	ds->data_observations.d = Calloc(mb->predictor_ndata, double);

	double *w = NULL;
	int nw = 0;
	inla_read_data_all(&w, &nw, ds->weight_file.name);
	if (nw) {
		// P(nw); P(ds->data_observations.ndata); P(mb->predictor_ndata);
		assert(nw == mb->predictor_ndata);
	}

	for (i = j = 0; i < n; i += idiv, j++) {
		ii = (int) x[i];
		if (!LEGAL(ii, mb->predictor_ndata)) {
			inla_error_file_error(__GMRFLib_FuncName, ds->data_file.name, n, i, x[i]);
		}
		for (k = 0; k < na; k++) {
			a[k][ii] = x[i + k + 1];
		}
		ds->data_observations.y[ii] = x[i + idiv - 1];
		if (w) {
			ds->data_observations.d[ii] = w[ii];
		} else {
			ds->data_observations.d[ii] = 1.0;
		}
		if (mb->verbose && j < PREVIEW) {
			switch (na) {
			case 0:
				printf("\t\t\t%1d/%1d  (idx,y,d) = (%1d, %g, %g)\n", j, ds->data_observations.ndata, ii, ds->data_observations.y[ii],
				       ds->data_observations.d[ii]);
				break;

			case 1:
				printf("\t\t\t%1d/%1d  (idx,a,y,d) = (%1d, %g, %g, %g)\n", j,
				       ds->data_observations.ndata, ii, a[0][ii], ds->data_observations.y[ii], ds->data_observations.d[ii]);
				break;

			case 2:
				printf("\t\t\t%1d/%1d (idx,a[0],a[1],y,d) = (%1d, %g, %g, %g, %g)\n", j,
				       ds->data_observations.ndata, ii, a[0][ii], a[1][ii], ds->data_observations.y[ii], ds->data_observations.d[ii]);
				break;

			case 3:
				printf("\t\t\t%1d/%1d (idx,a[0],a[1],a[2],y,d) = (%1d, %g, %g, %g, %g, %g)\n", j,
				       ds->data_observations.ndata, ii, a[0][ii], a[1][ii], a[2][ii], ds->data_observations.y[ii], ds->data_observations.d[ii]);
				break;

			case 4:
				printf("\t\t\t%1d/%1d (idx,a[0],a[1],a[2],a[3],y,d) = (%1d, %g, %g, %g, %g, %g, %g)\n", j,
				       ds->data_observations.ndata, ii, a[0][ii], a[1][ii], a[2][ii], a[3][ii], ds->data_observations.y[ii],
				       ds->data_observations.d[ii]);
				break;

			default:
				fprintf(stderr, "\n\n\nADD CODE HERE\n\n\n");
				exit(EXIT_FAILURE);
			}
		}
	}

	Free(w);
	Free(x);

	return INLA_OK;
}
int inla_read_data_general(double **xx, int **ix, int *nndata, const char *filename, int n, int column, int n_columns, int verbose, double default_value)
{
	/*
	 * read a column from file. the first (or first two) columns are indices and these are not counted.
	 *
	 * counting starts from 0. unread values are default set to DEFAULT_VALUE. n_columns is the total number of columns
	 * (except the indices).
	 *
	 * if xx exists, then read into xx. otherwise, read into ix. return number of reads, in *nx
	 */
	int nx, ndata, i, j, ii, ncol_true;
	double *x = NULL;

	assert(xx || ix);
	/*
	 * first read all entries in the file 
	 */
	inla_read_data_all(&x, &nx, filename);
	if (verbose) {
		printf("\t\tread n=[%1d] entries from file=[%s]\n", nx, filename);
	}
	ncol_true = n_columns + 1;
	if (!inla_divisible(nx, ncol_true)) {
		inla_error_file_numelm(__GMRFLib_FuncName, filename, nx, ncol_true);
	}
	ndata = nx / ncol_true;
	if (xx) {
		*xx = Calloc(n, double);
		for (i = 0; i < n; i++) {
			(*xx)[i] = default_value;
		}
	} else {
		*ix = Calloc(n, int);
		for (i = 0; i < n; i++) {
			(*ix)[i] = (int) default_value;
		}
	}
	for (i = j = 0; i < nx; i += ncol_true, j++) {
		ii = (int) x[i];
		if (!LEGAL(ii, n)) {
			inla_error_file_error(__GMRFLib_FuncName, filename, nx, i, x[i]);
		}
		if (xx) {
			(*xx)[ii] = x[i + column + 1];
		} else {
			(*ix)[ii] = (int) x[i + column + 1];
		}
		if (verbose && j < PREVIEW) {
			printf("\t\tfile=[%s] %1d/%1d  (idx,y) = (%1d, %g)\n", filename, j, ndata, ii, x[i + column + 1]);
		}
	}
	if (nndata) {
		*nndata = ndata;
	}
	return INLA_OK;
}
int loglikelihood_inla(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	inla_tp *a = (inla_tp *) arg;
	return a->loglikelihood[idx] (logll, x, m, idx, x_vec, a->loglikelihood_arg[idx]);
}
double inla_Phi(double x)
{
	/*
	 * the un-log version of inla_log_Phi 
	 */
	if (ABS(x) < 7.0) {
		return gsl_cdf_ugaussian_P(x);
	} else {
		return exp(inla_log_Phi(x));
	}
}
double inla_log_Phi(double x)
{
	// return the log of the cummulative distribution function for a standard normal.
	// This version is ok for all x 
	if (ABS(x) < 7.0) {
		return (log(gsl_cdf_ugaussian_P(x)));
	} else {
		double t1, t4, t3, t8, t9, t13, t27, t28, t31, t47;

		if (x >= 7.0) {
			t1 = 1.77245385090551602729816748334;
			t3 = M_SQRT2;
			t4 = t3 / t1;
			t8 = x * x;
			t9 = t8 * x;
			t13 = t8 * t8;
			t27 = exp(t8);
			t28 = sqrt(t27);
			t31 = 0.1e1 / M_PI;
			t47 = 0.1e1 / t28 * (-0.1e1 / x * t4 / 0.2e1 + 0.1e1 / t9 * t4 / 0.2e1 - 0.3e1 / 0.2e1 / t13 / x * t4 + 0.15e2 / 0.2e1 / t13 / t9 * t4)
			    + 0.1e1 / t27 * (-0.1e1 / t8 * t31 / 0.4e1 + 0.1e1 / t13 * t31 / 0.2e1 - 0.7e1 / 0.4e1 / t13 / t8 * t31);
			return t47;
		} else {
			// x <= -7.0
			double xx = -x, cg1;
			cg1 = -(pow(xx, 0.6e1) + log(0.2e1) * pow(xx, 0.4e1) + log(0.3141592653589793e1) * pow(xx, 0.4e1) + 0.2e1 * log(xx) * pow(xx, 0.4e1)
				- 0.5e1 + 0.2e1 * xx * xx) * pow(xx, -0.4e1) / 0.2e1;
			return (cg1);
		}
	}
	abort();
	return 0;
}
double inla_Phi_fast(double x)
{
	// a faster approximation, see misc/doc/doc/approximate-cdf-normal.pdf
	if (ABS(x) < 7.0) {
		// see misc/doc/doc/approximate-cdf-normal.pdf
		if (x > 0.0) {
			return (0.5 + 0.5 * sqrt(1.0 - exp(-sqrt(M_PI / 8.0) * SQR(x))));
		} else {
			return (1.0 - (0.5 + 0.5 * sqrt(1.0 - exp(-sqrt(M_PI / 8.0) * SQR(x)))));
		}
		return (0.5 + 0.5 * sqrt(1.0 - exp(-sqrt(M_PI / 8.0) * SQR(x))));
	} else {
		return inla_Phi(x);
	}
}
double inla_log_Phi_fast(double x)
{
	// a faster approximation, see misc/doc/doc/approximate-cdf-normal.pdf
	if (ABS(x) < 7.0) {
		return (log(inla_Phi_fast(x)));
	} else {
		if (x > 7.0) {
			return (-1.0 / 4.0 * exp(-sqrt(M_PI / 8.0) * SQR(x)));
		} else {
			return (log(1.0 / 4.0) - sqrt(M_PI / 8.0) * SQR(x));
		}
	}
}
double laplace_likelihood_normalising_constant(double alpha, double ggamma, double tau)
{
#define N 200
#define f_positive(x) exp(-tau*log(cosh(alpha*ggamma*x))/ggamma)
#define f_negative(x) exp(-tau*log(cosh((1.0-alpha)*ggamma*x))/ggamma)

	static double alpha_save = -1.0, ggamma_save = -1.0, tau_save = -1.0, norm_const_save = 1.0;
#pragma omp threadprivate(alpha_save, ggamma_save, tau_save, norm_const_save)

	if (!ISZERO(alpha - alpha_save) || !ISZERO(ggamma - ggamma_save) || !ISZERO(tau - tau_save)) {

		alpha_save = alpha;
		ggamma_save = ggamma;
		tau_save = tau;

		int i, k;
		double limit = 1.0e-10, x_upper, x_lower, xx[N], w[2] = { 4.0, 2.0 }, integral_positive = 0.0, integral_negative = 0.0, dx;

		for (x_upper = 0.0;; x_upper += 1.0 / (tau * alpha)) {
			if (f_positive(x_upper) < limit)
				break;
		}

		dx = x_upper / (N - 1.0);
		for (i = 0; i < N; i++)
			xx[i] = dx * i;

		integral_positive = f_positive(xx[0]) + f_positive(xx[N - 1]);
		for (i = 1, k = 0; i < N - 1; i++, k = (k + 1) % 2) {
			integral_positive += f_positive(xx[i]) * w[k];
		}
		integral_positive *= dx / 3.0;

		for (x_lower = 0.0;; x_lower -= 1.0 / ((1.0 - alpha) * tau)) {
			if (f_negative(x_lower) < limit)
				break;
		}
		dx = -x_lower / (N - 1.0);
		for (i = 0; i < N; i++)
			xx[i] = -dx * i;

		integral_negative = f_negative(xx[0]) + f_negative(xx[N - 1]);
		for (i = 1, k = 0; i < N - 1; i++, k = (k + 1) % 2) {
			integral_negative += f_negative(xx[i]) * w[k];
		}
		integral_negative *= dx / 3.0;

		norm_const_save = 1.0 / (integral_negative + integral_positive);
	}
#undef N
#undef f_postive
#undef f_negative

	return norm_const_save;
}
int loglikelihood_laplace(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * asymmetric Laplace: f(u) = prec*alpha*(1-alpha)*exp(-prec*g(u)) where g(u) = -alpha*u, if u > 0, and g(u) = -(1-alpha)*|u|, if u < 0. we approximate this
	 * using -log(cosh(ggamma*u))/ggamma, where the approximation improves for increasing ggamma. ggamma=1 by default.
	 */

	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, tau, w, alpha, u, lnc, epsilon, ggamma, a, ypred;

	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_laplace[idx];
	tau = map_tau_laplace(ds->data_observations.log_tau_laplace[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;
	alpha = ds->data_observations.alpha_laplace;
	epsilon = ds->data_observations.epsilon_laplace;
	ggamma = ds->data_observations.gamma_laplace;
	lnc = log(laplace_likelihood_normalising_constant(alpha, ggamma, tau));

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			u = y - ypred;
			if (u > 0) {
				a = alpha;
			} else {
				a = 1.0 - alpha;
				u = ABS(u);
			}
			logll[i] = lnc - tau / ggamma * (-M_LN2 + ggamma * a * u + log(1.0 + exp(-2.0 * ggamma * a * u))) - 0.5 * tau * epsilon * SQR(a * u);
		}
	}
	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_gaussian(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Normal(x, stdev)
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, lprec, prec, w, ypred;

	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_gaussian[idx];
	lprec = ds->data_observations.log_prec_gaussian[GMRFLib_thread_id][0] + log(w);
	prec = map_precision(ds->data_observations.log_prec_gaussian[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = LOG_NORMC_GAUSSIAN + 0.5 * (lprec - (SQR(ypred - y) * prec));
		}
	} else {
		for (i = 0; i < -m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = inla_Phi((y - ypred) * sqrt(prec));
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_simplex(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ simplex
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, yy, ypyp, lprec, prec, w, ypred;

	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_simplex[idx];
	lprec = ds->data_observations.log_prec_simplex[GMRFLib_thread_id][0] + log(w);
	prec = map_precision(ds->data_observations.log_prec_simplex[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			yy = y * (1.0 - y);
			ypyp = ypred * (1.0 - ypred);
			logll[i] = 0.5 * lprec - 0.5 * (log(2.0 * M_PI) + 3.0 * log(yy))
			    - prec * SQR(y - ypred) / (2.0 * yy * SQR(ypyp));
		}
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);

	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_circular_normal(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ circular normal
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, prec, w, ypred;

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_circular_normal[idx];
	prec = map_precision(ds->data_observations.log_prec_circular_normal[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;

	/*
	 * store the normalising constant as it involves bessel_I0: -log(2 Pi BesselI0(kappa)),
	 * which is ok as long as the scalings 'w' do not change to often.
	 */
	static double log_norm_const = 0.0, log_norm_const_arg = DBL_MAX;
#pragma omp threadprivate(log_norm_const, log_norm_const_arg)

	if (!ISEQUAL(prec, log_norm_const_arg)) {
		log_norm_const_arg = prec;
		log_norm_const = -log(2.0 * M_PI * gsl_sf_bessel_I0(prec));
	}

	for (i = 0; i < m; i++) {
		ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));

		/*
		 * we need |y-ypred| <= Pi, but this might not be the case...  so we add a penalty if this condition is not met
		 */
		if (ABS(y - ypred) <= M_PI) {
			logll[i] = log_norm_const + prec * cos(y - ypred);
		} else {
			double penalty = 1.0e8 * prec;
			if (y - ypred > M_PI) {
				logll[i] = log_norm_const + prec * cos(M_PI) - penalty / 2.0 * SQR(y - ypred - M_PI);
			} else {
				logll[i] = log_norm_const + prec * cos(-M_PI) - penalty / 2.0 * SQR(y - ypred + M_PI);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_wrapped_cauchy(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ wrapped cauchy. DOES NOT WORK WELL OF'COURSE...
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, rho, rho2, w, ypred;
	double mlog2pi = -1.8378770664093454836;	       /* -log(2*pi) */

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_wrapped_cauchy[idx];
	rho = map_probability(ds->data_observations.log_prec_wrapped_cauchy[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;
	rho2 = SQR(rho);

	for (i = 0; i < m; i++) {
		ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));

		/*
		 * we need |y-ypred| <= Pi, but this might not be the case...  so we add a penalty if this condition is not met
		 */
		if (ABS(y - ypred) <= M_PI) {
			logll[i] = mlog2pi + log(1.0 - rho2) - log(1.0 + rho2 - 2.0 * rho * cos(y - ypred));
		} else {
			double penalty = 1.0e6 * (2.0 * rho / SQR(1.0 - rho));	/* The -Hessian in the mode... */
			if (y - ypred > M_PI) {
				logll[i] = mlog2pi + log(1.0 - rho2) - log(1.0 + rho2 - 2.0 * rho * cos(M_PI))
				    - penalty / 2.0 * SQR(y - ypred - M_PI);
			} else {
				logll[i] = mlog2pi + log(1.0 - rho2) - log(1.0 + rho2 - 2.0 * rho * cos(-M_PI))
				    - penalty / 2.0 * SQR(y - ypred + M_PI);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_iid_gamma(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ iid_gamma
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double shape, rate, w, xx, penalty = 1.0 / FLT_EPSILON, cons;

	LINK_INIT;
	w = ds->data_observations.iid_gamma_weight[idx];
	shape = map_exp(ds->data_observations.iid_gamma_log_shape[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	rate = map_exp(ds->data_observations.iid_gamma_log_rate[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;
	cons = -shape * log(rate) - gsl_sf_lngamma(shape);

	if (m > 0) {
		for (i = 0; i < m; i++) {
			xx = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			if (xx > FLT_EPSILON) {
				logll[i] = cons + (shape - 1.0) * log(xx) - rate * xx;
			} else {
				/*
				 * this is the penalty, and should not happen in the end... 
				 */
				logll[i] = cons + (shape - 1.0) * log(FLT_EPSILON) - rate * FLT_EPSILON - penalty * SQR(FLT_EPSILON - xx);
			}
		}
	}
	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_iid_logitbeta(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ iid_logitbeta
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double a, b, xx, cons, eta;

	LINK_INIT;
	a = map_exp(ds->data_observations.iid_logitbeta_log_a[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	b = map_exp(ds->data_observations.iid_logitbeta_log_b[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	cons = gsl_sf_lngamma(a + b) - (gsl_sf_lngamma(a) + gsl_sf_lngamma(b));

	if (m > 0) {
		for (i = 0; i < m; i++) {
			eta = x[i] + OFFSET(idx);
			xx = PREDICTOR_INVERSE_LINK(eta);
			logll[i] = cons + (a - 1.0) * log(xx) + (b - 1.0) * log(1.0 - xx) + PREDICTOR_INVERSE_LINK_LOGJACOBIAN(eta);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_loggamma_frailty(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * Log-gamma frailty Gamma(a,a), a = exp(log_prec...)
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double lprec, prec;
	double log_gamma;

	LINK_INIT;
	lprec = ds->data_observations.log_prec_loggamma_frailty[GMRFLib_thread_id][0];
	prec = map_precision(lprec, MAP_FORWARD, NULL);
	log_gamma = gsl_sf_lngamma(prec);

	if (m > 0) {
		for (i = 0; i < m; i++) {
			logll[i] = -log_gamma + prec * (lprec + x[i] - exp(x[i]));
		}
	}
	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_logistic(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Logisistc. scaled so that prec = 1 gives variance = 1
	 *
	 * A := Pi/sqrt(3)
	 *
	 * > F(x);             
	 *                                                     1
	 *                                          ------------------------
	 *                                          1 + exp(-tau A (x - mu))
	 *
	 *
	 * > solve(F(x) = p,x);
	 *                                                         -1 + p
	 *                                           tau A mu - ln(- ------)
	 *                                                             p
	 *                                           -----------------------
	 *                                                    tau A
	 * > diff(F(x),x);
	 *                                          tau A exp(-tau A (x - mu))
	 *                                         ---------------------------
	 *                                                                   2
	 *                                         (1 + exp(-tau A (x - mu)))
	 *
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, prec, w, A = M_PI / sqrt(3.0), precA, lprecA, eta;

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_logistic[idx];
	prec = map_precision(ds->data_observations.log_prec_logistic[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;
	precA = prec * A;
	lprecA = log(precA);

	if (m > 0) {
		for (i = 0; i < m; i++) {
			eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = lprecA - precA * (y - eta) - 2.0 * log(1.0 + exp(-precA * (y - eta)));
		}
	} else {
		for (i = 0; i < -m; i++) {
			eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = 1.0 / (1.0 + exp(-precA * (y - eta)));
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_skew_normal(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Skew_Normal(x, stdev)
	 */
	if (m == 0) {
		return 0;				       /* no features available for now */
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, lprec, sprec, w, shape, shape_max, xarg, ypred;

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_skew_normal[idx];
	lprec = ds->data_observations.log_prec_skew_normal[GMRFLib_thread_id][0] + log(w);
	sprec = sqrt(map_precision(ds->data_observations.log_prec_skew_normal[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w);
	shape_max = ds->data_observations.shape_max_skew_normal;
	shape = map_rho(ds->data_observations.shape_skew_normal[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * shape_max;

	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			xarg = (y - ypred) * sprec;
			logll[i] = LOG_NORMC_GAUSSIAN + M_LOG2E + 0.5 * (lprec - SQR(xarg)) + inla_log_Phi_fast(shape * xarg);
		}
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_skew_normal2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Skew_Normal(x, stdev), with the moment parameterisation
	 */

	// skewmx = 0.995271746...
#define SKEW_MAX ((4.0-M_PI)/2.0/pow(ABS(1.0-M_PI/2), 3.0/2.0))
#define SIGN(_x) ((_x) >= 0.0 ? 1.0 : -1.0)

	if (m == 0) {
		return 0;				       /* no features available for now */
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, lprec, w, skewness, xarg, ypred, xi, omega, alpha, g, variance, delta;

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_skew_normal2[idx];
	lprec = ds->data_observations.log_prec_skew_normal2[GMRFLib_thread_id][0] + log(w);
	variance = exp(-lprec);
	skewness = map_rho(ds->data_observations.logit_skewness_skew_normal2[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			if (ISZERO(skewness)) {
				g = 0.0;
			} else {
				g = M_PI / (2.0 * (1.0 + pow((4.0 - M_PI) / (2.0 * ABS(skewness)), 2.0 / 3.0)));
				g = DMIN(SKEW_MAX - GMRFLib_eps(0.75), g);
				g = DMAX(-(SKEW_MAX - GMRFLib_eps(0.75)), g);
			}
			alpha = SIGN(skewness) * sqrt(g / (1.0 - g));
			delta = alpha / sqrt(1.0 + SQR(alpha));
			omega = sqrt(variance / (1.0 - 2.0 * SQR(delta) / M_PI));
			// Do not do this anymore...
			// xi = ypred - omega * delta * sqrt(2.0 / M_PI); 
			xi = ypred;
			xarg = (y - xi) / omega;
			logll[i] = LOG_NORMC_GAUSSIAN + M_LOG2E - log(omega) - 0.5 * SQR(xarg) + inla_log_Phi_fast(alpha * xarg);
		}
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

#undef SKEW_MAX
#undef SIGN
	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_gev(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ GEV
	 */
	int i;
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, sprec, w, xi, xx, ypred;

	LINK_INIT;
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_gev[idx];
	sprec = sqrt(map_precision(ds->data_observations.log_prec_gev[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w);
	/*
	 * map_identity_scale(theta, MAP_FORWARD, arg...);
	 */
	xi = ds->data_observations.gev_scale_xi * ds->data_observations.xi_gev[GMRFLib_thread_id][0];

	if (m > 0) {
		if (ISZERO(xi)) {
			for (i = 0; i < m; i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				xx = sprec * (y - ypred);
				logll[i] = -xx - exp(-xx) + log(sprec);
			}
		} else {
			for (i = 0; i < m; i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				xx = 1.0 + xi * sprec * (y - ypred);
				if (xx > DBL_EPSILON) {
					logll[i] = (-1.0 / xi - 1.0) * log(xx) - pow(xx, -1.0 / xi) + log(sprec);
				} else {
					logll[i] =
					    (-1.0 / xi - 1.0) * log(DBL_EPSILON) - pow(DBL_EPSILON, -1.0 / xi) + log(sprec) - 1e6 * SQR(sprec * (DBL_EPSILON - xx));
				}
			}
		}
	} else {
		if (ISZERO(xi)) {
			for (i = 0; i < -m; i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				xx = sprec * (y - ypred);
				logll[i] = exp(-exp(-xx));
			}
		} else {
			for (i = 0; i < -m; i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				xx = sprec * (y - ypred);
				if (xi > 0.0) {
					if (1.0 + xi * xx > 0.0) {
						logll[i] = exp(-pow(xx, -xi));
					} else {
						logll[i] = 0.0;
					}
				} else {
					if (1.0 + xi * xx > 0.0) {
						logll[i] = exp(-pow(xx, xi));
					} else {
						logll[i] = 1.0;
					}
				}
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_t(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y -x ~ (Student_t with variance 1) times 1/sqrt(precision * weight)
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, prec, w, dof, y_std, fac, ypred;

	LINK_INIT;
	dof = map_dof(ds->data_observations.dof_intern_t[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_t[idx];
	prec = map_precision(ds->data_observations.log_prec_t[GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;
	fac = sqrt((dof / (dof - 2.0)) * prec);

	// cache the values of lg1 and lg2
	static double lg1 = 0.0, lg2 = 0.0, dof_cache = -1.0;
#pragma omp threadprivate (lg1, lg2, dof_cache)
	if (dof != dof_cache) {
		dof_cache = dof;
		lg1 = gsl_sf_lngamma(dof / 2.0);
		lg2 = gsl_sf_lngamma((dof + 1.0) / 2.0);
	}

	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			y_std = (y - ypred) * fac;
			// logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(y_std) / dof) + log(fac);
			logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * gsl_log1p(SQR(y_std) / dof) + log(fac);
		}
	} else {
		for (i = 0; i < -m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = gsl_cdf_tdist_P((y - ypred) * fac, dof);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_tstrata(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y -x ~ (Student_t with variance 1) times 1/sqrt(precision * weight)
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_DERIVATIES_AND_CDF;
	}

	int i, dcode, strata;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y, prec, w, dof, y_std, fac, ypred;

	int bit_fac = GMRFLib_getbit((GMRFLib_uchar) ds->variant, (unsigned int) 0);
	int bit_tail = GMRFLib_getbit((GMRFLib_uchar) ds->variant, (unsigned int) 1);

	LINK_INIT;
	dof = map_dof(ds->data_observations.dof_intern_tstrata[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	y = ds->data_observations.y[idx];
	w = ds->data_observations.weight_tstrata[idx];
	strata = (int) (ds->data_observations.strata_tstrata[idx] + FLT_EPSILON);
	prec = map_precision(ds->data_observations.log_prec_tstrata[strata][GMRFLib_thread_id][0], MAP_FORWARD, NULL) * w;

	// printf("idx y x strata prec %d %g %g %d %g\n", idx, y, x[0], strata, prec);

	switch (bit_fac) {
	case 0:
		fac = sqrt(prec);
		break;

	case 1:
		fac = sqrt((dof / (dof - 2.0)) * prec);
		break;

	default:
		assert(0 == 1);
	}


	// cache the values of lg1 and lg2
	static double lg1 = 0.0, lg2 = 0.0, dof_cache = -1.0;
#pragma omp threadprivate (lg1, lg2, dof_cache)
	if (dof != dof_cache) {
		dof_cache = dof;
		lg1 = gsl_sf_lngamma(dof / 2.0);
		lg2 = gsl_sf_lngamma((dof + 1.0) / 2.0);
	}

	int use_tail_correction;

	switch (bit_tail) {
	case 0:
		use_tail_correction = GMRFLib_FALSE;
		break;

	case 1:
		use_tail_correction = GMRFLib_TRUE;
		break;

	default:
		assert(0 == 1);
	}

	double tail_factor = 0.98;
	double tail_start = tail_factor * sqrt(dof);
	double tail_prec = (dof + 1.0) * (dof - SQR(tail_start)) / SQR(dof + SQR(tail_start));
	double diff = -(dof + 1) * tail_start / (dof + SQR(tail_start));
	double dev, log_normc, normc1, log_normc2, eff, ef;

	if (m > 0) {
		if (use_tail_correction) {
			normc1 = 2.0 * gsl_cdf_tdist_P(tail_start, dof) - 1.0;
			log_normc2 = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(tail_start) / dof) + log(fac)
			    - 0.5 * log(2.0) + 0.5 * log(M_PI / tail_prec)
			    + 0.5 * SQR(diff) / tail_prec;
			eff = diff / sqrt(2.0 * tail_prec);
			ef = gsl_sf_erf(eff);
			if (ef == -1.0) {
				/*
				 * asymptotic expansion 
				 */
				log_normc2 += -SQR(eff) - 0.5 * log(M_PI) - log(-eff) - 1.0 / (2.0 * SQR(eff));
			} else if (ef == 1.0) {
				/*
				 * asymptotic expansion 
				 */
				log_normc2 += log(2.0) - 1.0 / (2.0 * sqrt(M_PI) * eff) * exp(-SQR(eff));
			} else {
				log_normc2 += log(ef + 1.0);
			}
			log_normc = log(normc1 + 2.0 * exp(log_normc2));
		} else {
			log_normc = 0.0;
		}

		if (m > 0) {
			/*
			 * assume this for the moment, otherwise we have to add new code...
			 */
			assert(ds->predictor_invlinkfunc == link_identity);

			for (i = 0; i < m; i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				y_std = (y - ypred) * fac;
				dcode = (m <= 3 ? i : 0);      /* if m > 3 we should not compute deriviaties... */

				if (ABS(y_std) > tail_start && use_tail_correction) {
					if (y_std > tail_start) {
						dev = y_std - tail_start;
					} else {
						dev = y_std + tail_start;
						diff *= -1.0;  /* swap sign */
					}

					switch (dcode) {
					case 0:
						logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(tail_start) / dof) + log(fac);
						logll[i] += -0.5 * tail_prec * SQR(dev) + diff * dev;
						logll[i] -= log_normc;
						break;

					case 1:
						if (y_std > tail_start) {
							logll[i] = tail_prec * dev * fac - diff * fac;
						} else {
							logll[i] = tail_prec * dev * fac + diff * fac;
						}
						break;

					case 2:
						logll[i] = -tail_prec * SQR(fac);
						break;

					default:
						assert(0 == 1);
					}
				} else {
					switch (dcode) {
					case 0:
						logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(y_std) / dof) + log(fac);
						logll[i] -= log_normc;
						break;

					case 1:
						logll[i] = (dof + 1.0) * fac * y_std / (dof + SQR(y_std));
						break;

					case 2:
						logll[i] = -(dof + 1.0) * SQR(fac) * (dof - SQR(y_std)) / SQR(dof + SQR(y_std));
						break;

					default:
						assert(0 == 1);
					}
				}

			}
		} else {
			for (i = 0; i < (-m); i++) {
				ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				y_std = (y - ypred) * fac;
				if (ABS(y_std) > tail_start && use_tail_correction) {
					if (y_std > tail_start) {
						dev = y_std - tail_start;
					} else {
						dev = y_std + tail_start;
						diff *= -1.0;  /* swap sign */
					}
					logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(tail_start) / dof) + log(fac);
					logll[i] += -0.5 * tail_prec * SQR(dev) + diff * dev;
				} else {
					logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(y_std) / dof) + log(fac);
				}
				logll[i] -= log_normc;
			}
		}
	} else {
		FIXME1("PIT-VALUES ARE NOT YET CORRECT AND ASSUME T.");
		for (i = 0; i < -m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = gsl_cdf_tdist_P((y - ypred) * fac, dof);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_gpoisson(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ GPoisson(E*exp(x))
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i, yy;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx];
	double log_y_fact = gsl_sf_lnfact((unsigned int) y);
	double phi = map_exp(ds->data_observations.gpoisson_overdispersion[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double p = map_identity(ds->data_observations.gpoisson_p[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double E = ds->data_observations.E[idx];
	double a, b, lambda, mu;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			a = mu + phi * pow(mu, p - 1.0) * y;
			b = 1 + phi * pow(mu, p - 1.0);
			logll[i] = log(mu) + (y - 1.0) * log(a) - y * log(b) - log_y_fact - a / b;
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			b = 1 + phi * pow(mu, p - 1.0);
			logll[i] = 0.0;
			for (yy = 0; yy <= (int) y; yy++) {
				a = mu + phi * pow(mu, p - 1.0) * yy;
				logll[i] += exp(log(mu) + (yy - 1.0) * log(a) - yy * log(b) - gsl_sf_lnfact((unsigned int) yy) - a / b);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_poisson(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
#define logE(E_) (E_ > 0.0 ? log(E_) : 0.0)

	/*
	 * y ~ Poisson(E*exp(x)), also accept E=0, giving the likelihood y * x.
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], E = ds->data_observations.E[idx], normc = gsl_sf_lnfact((unsigned int) y), lambda;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = y * (log(lambda) + logE(E)) - E * lambda - normc;
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			if (ISZERO(E * lambda)) {
				if (ISZERO(y)) {
					logll[i] = 1.0;
				} else {
					assert(!ISZERO(y));
				}
			} else {
				logll[i] = gsl_cdf_poisson_P((unsigned int) y, E * lambda);
			}
		}
	}

	LINK_END;
#undef logE
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_poisson0(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Poission: y ~ p*1[y=0] + (1-p)*Poisson(E*exp(x) | y > 0)
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], E = ds->data_observations.E[idx], normc = gsl_sf_lnfact((unsigned int) y),
	    p = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), mu, lambda;

	LINK_INIT;
	if ((int) y == 0) {
		/*
		 * this is just the point-mass at zero 
		 */
		if (m > 0) {
			for (i = 0; i < m; i++) {
				logll[i] = log(p);
			}
		} else {
			for (i = 0; i < -m; i++) {
				logll[i] = p;
			}
		}
	} else {
		/*
		 * As for the Poisson but '|y>0', and Prob(y > 0) = 1-exp(-mean) 
		 */
		if (m > 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logll[i] = log(1.0 - p) + y * log(mu) - mu - normc - log(1.0 - exp(-mu));
			}
		} else {
			for (i = 0; i < -m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logll[i] = p + (1.0 - p) * (gsl_cdf_poisson_P((unsigned int) y, mu) - gsl_cdf_poisson_P((unsigned int) 0, mu));
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_poisson1(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Poission: y ~ p*1[y=0] + (1-p)*Poisson(E*exp(x))
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], E = ds->data_observations.E[idx], normc = gsl_sf_lnfact((unsigned int) y),
	    p = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), mu, lambda, logA, logB;

	LINK_INIT;
	if ((int) y == 0) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logA = log(p);
				logB = log(1.0 - p) + y * log(mu) - mu - normc;
				// logll[i] = log(p + (1.0 - p) * gsl_ran_poisson_pdf((unsigned int) y, mu));
				logll[i] = eval_logsum_safe(logA, logB);
			}
		} else {
			for (i = 0; i < -m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logll[i] = p + (1.0 - p) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	} else {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logll[i] = log(1.0 - p) + y * log(mu) - mu - normc;
			}
		} else {
			for (i = 0; i < -m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				logll[i] = p + (1.0 - p) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_poisson2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Poission: y ~ p*1[y=0] + (1-p)*Poisson(E*exp(x)), where p=p(x; alpha)
	 */

#define PROB(xx, EE) (1.0-pow(EE*exp(xx)/(1.0+EE*exp(xx)), alpha))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], E = ds->data_observations.E[idx], normc = gsl_sf_lnfact((unsigned int) y),
	    alpha = map_exp(ds->data_observations.zeroinflated_alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), mu, log_mu, p, lambda;

	LINK_INIT;
	if ((int) y == 0) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				p = PROB(x[i] + OFFSET(idx), E);
				if (gsl_isnan(p)) {
					// P(p);
					// P(x[i]+OFFSET(idx));
					logll[i] = 0.0;
				} else {
					lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
					mu = E * lambda;
					log_mu = log(mu);

					// better expression I hope

					if (ISEQUAL(p, 1.0)) {
						logll[i] = 0.0;
					} else if (p < 1e-10) {
						logll[i] = 0 * log_mu - mu - normc;
					} else {
						logll[i] = 0 * log_mu - mu - normc + log(p / (gsl_ran_poisson_pdf((unsigned int) y, mu)) + (1.0 - p));
					}
					// logll[i] = log(p + (1.0 - p) * gsl_ran_poisson_pdf((unsigned int) y, mu));

					/*
					 * if all fails... 
					 */
					if (gsl_isnan(logll[i])) {
						P(p);
						P(logll[i]);
						P(x[i] + OFFSET(idx));
						fprintf(stderr, "inla.c: Don't know what to do. Please report problem...");
						exit(EXIT_FAILURE);
					}
				}
			}
		} else {
			for (i = 0; i < -m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				p = PROB(x[i] + OFFSET(idx), E);
				logll[i] = p + (1.0 - p) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	} else {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				p = PROB(x[i] + OFFSET(idx), E);
				if (gsl_isnan(p)) {
					logll[i] = -DBL_MAX;
				} else {
					lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
					mu = E * lambda;
					log_mu = log(mu);
					logll[i] = log(1.0 - p) + y * log_mu - mu - normc;
				}
			}
		} else {
			for (i = 0; i < -m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				p = PROB(x[i] + OFFSET(idx), E);
				logll[i] = p + (1.0 - p) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	for (i = 0; i < IABS(m); i++) {
		if (gsl_isinf(logll[i]))
			logll[i] = ((double) FLT_MAX) * gsl_isinf(logll[i]);
	}

	LINK_END;
#undef PROB
	return GMRFLib_SUCCESS;
}
double exp_taylor(double x, double x0, int order)
{
	int i;
	double val = 1.0, xx = x - x0;

	assert(order > 0);
	for (i = order; i >= 1; i--) {
		val = 1.0 + val * xx / (double) i;
	}
	val *= exp(x0);
	return val;
}
double dexp_taylor(double x, double x0, int order)
{
	return exp_taylor(x, x0, order - 1);
}
double ddexp_taylor(double x, double x0, int order)
{
	return exp_taylor(x, x0, order - 2);
}
int loglikelihood_logperiodogram(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y = x -log(2) + log\chi^2
	 *
	 * use the exact expression or a taylor-series for the exp-term of a given order ??
	 */
	int order = 12, i;
	double x0 = 0.0;

	if (m == 0) {
		return 0;
	}
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], v, ypred;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			ypred = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			v = y - ypred + M_LOG2E;
			logll[i] = -M_LOG2E + v - 0.5 * exp_taylor(v, x0, order);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_negative_binomial(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double size;
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */

	switch (ds->variant) {
	case 0:
		size = exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
		break;
	case 1:
		size = E * exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
		break;
	default:
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}


	LINK_INIT;
	if (m > 0) {
		lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);	/* near always the case we'll need this one */
		for (i = 0; i < m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = lnorm + size * log(p) + y * log(1.0 - p);
			} else {
				/*
				 * 
				 * * the Poission limit 
				 */
				logll[i] = y * log(mu) - mu - gsl_sf_lnfact((unsigned int) y);
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = gsl_cdf_negative_binomial_P((unsigned int) y, p, size);
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_negative_binomial0(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.  This version is
	 * zeroinflated type 0.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double size = exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
	double p_zeroinflated = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, prob_y_is_zero, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */

	LINK_INIT;
	if (m > 0) {
		if ((int) y == 0) {
			for (i = 0; i < m; i++) {
				logll[i] = log(p_zeroinflated);
			}
		} else {
			/*
			 * this is constant for the NegativeBinomial 
			 */
			lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);

			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					prob_y_is_zero = gsl_ran_negative_binomial_pdf((unsigned int) 0, p, size);

					logll[i] = log((1.0 - p_zeroinflated) / (1.0 - prob_y_is_zero))
					    + lnorm + size * log(p) + y * log(1.0 - p);
				} else {
					/*
					 * the Poission limit 
					 */
					prob_y_is_zero = gsl_ran_poisson_pdf((unsigned int) 0, mu);
					logll[i] = log((1.0 - p_zeroinflated) / (1.0 - prob_y_is_zero))
					    + y * log(mu) - mu - gsl_sf_lnfact((unsigned int) y);
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) *
				    (gsl_cdf_negative_binomial_P((unsigned int) y, p, size) - gsl_cdf_negative_binomial_P((unsigned int) 0, p, size));
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) *
				    (gsl_cdf_poisson_P((unsigned int) y, mu) - gsl_cdf_poisson_P((unsigned int) 0, mu));
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_negative_binomial1(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.  This version is
	 * zeroinflated type 1.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double size = exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
	double p_zeroinflated = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */

	LINK_INIT;
	if (m > 0) {
		/*
		 * this is constant for the NegativeBinomial 
		 */
		lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);

		if ((int) y == 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_negative_binomial_pdf((unsigned int) y, p, size));
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_poisson_pdf((unsigned int) y, mu));
				}
			}
		} else {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(1.0 - p_zeroinflated) + lnorm + size * log(p) + y * log(1.0 - p);
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(1.0 - p_zeroinflated) + y * log(mu) - mu - gsl_sf_lnfact((unsigned int) y);
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_negative_binomial_P((unsigned int) y, p, size);
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_negative_binomial1_strata2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.  This version is
	 * zeroinflated type 1.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, strata = (int) ds->data_observations.strata[idx];
	double size = exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
	double p_zeroinflated;
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */

	if (strata == 0) {
		p_zeroinflated = map_probability(ds->data_observations.prob1_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	} else {
		p_zeroinflated = map_probability(ds->data_observations.prob2_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	}

	/*
	 * THE REST IS THE SAME AS FOR STRATA3 
	 */

	LINK_INIT;
	if (m > 0) {
		/*
		 * this is constant for the NegativeBinomial 
		 */
		lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);

		if ((int) y == 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_negative_binomial_pdf((unsigned int) y, p, size));
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_poisson_pdf((unsigned int) y, mu));
				}
			}
		} else {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(1.0 - p_zeroinflated) + lnorm + size * log(p) + y * log(1.0 - p);
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(1.0 - p_zeroinflated) + y * log(mu) - mu - gsl_sf_lnfact((unsigned int) y);
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_negative_binomial_P((unsigned int) y, p, size);
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_negative_binomial1_strata3(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.  This version is
	 * zeroinflated type 1.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, strata = (int) ds->data_observations.strata[idx];
	double size;
	double p_zeroinflated;
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */

	p_zeroinflated = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	if (strata == 0) {
		size = exp(ds->data_observations.log_sizes[0][GMRFLib_thread_id][0]);
	} else {
		size = exp(ds->data_observations.log_sizes[1][GMRFLib_thread_id][0]);
	}

	/*
	 * THE REST IS THE SAME AS FOR STRATA2 
	 */


	LINK_INIT;
	if (m > 0) {
		/*
		 * this is constant for the NegativeBinomial 
		 */
		lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);

		if ((int) y == 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_negative_binomial_pdf((unsigned int) y, p, size));
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_poisson_pdf((unsigned int) y, mu));
				}
			}
		} else {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				if (mu / size > cutoff) {
					/*
					 * NegativeBinomial 
					 */
					p = size / (size + mu);
					logll[i] = log(1.0 - p_zeroinflated) + lnorm + size * log(p) + y * log(1.0 - p);
				} else {
					/*
					 * the Poission limit 
					 */
					logll[i] = log(1.0 - p_zeroinflated) + y * log(mu) - mu - gsl_sf_lnfact((unsigned int) y);
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_negative_binomial_P((unsigned int) y, p, size);
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_negative_binomial2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ NegativeBinomial(size, p) where E(y) = E*exp(x); same definition as in R and GSL, similar parameterisation as for the Poisson.  This version is
	 * zeroinflated type 3.
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double size = exp(ds->data_observations.log_size[GMRFLib_thread_id][0]);
	double alpha = map_exp(ds->data_observations.zeroinflated_alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double p_zeroinflated = 0.0;
	double y = ds->data_observations.y[idx];
	double E = ds->data_observations.E[idx];
	double lnorm, mu, p, lambda;
	double cutoff = 1.0e-4;				       /* switch to Poisson if mu/size < cutoff */
	double normc = gsl_sf_lnfact((unsigned int) y);

	LINK_INIT;
	if (m > 0) {
		/*
		 * this is constant for the NegativeBinomial 
		 */
		lnorm = gsl_sf_lngamma(y + size) - gsl_sf_lngamma(size) - gsl_sf_lngamma(y + 1.0);

		if ((int) y == 0) {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				p_zeroinflated = 1.0 - pow(mu / (1.0 + mu), alpha);

				if (gsl_isnan(p_zeroinflated)) {
					logll[i] = -DBL_MAX;
				} else {
					if (mu / size > cutoff) {
						/*
						 * NegativeBinomial 
						 */
						p = size / (size + mu);
						logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_negative_binomial_pdf((unsigned int) y, p, size));
					} else {
						/*
						 * the Poission limit 
						 */
						logll[i] = log(p_zeroinflated + (1.0 - p_zeroinflated) * gsl_ran_poisson_pdf((unsigned int) y, mu));
					}
				}
			}
		} else {
			for (i = 0; i < m; i++) {
				lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				mu = E * lambda;
				p_zeroinflated = 1.0 - pow(mu / (1.0 + mu), alpha);
				if (gsl_isnan(p_zeroinflated)) {
					logll[i] = -DBL_MAX;
				} else {
					if (mu / size > cutoff) {
						/*
						 * NegativeBinomial 
						 */
						p = size / (size + mu);
						logll[i] = log(1.0 - p_zeroinflated) + lnorm + size * log(p) + y * log(1.0 - p);
					} else {
						/*
						 * the Poission limit 
						 */
						logll[i] = log(1.0 - p_zeroinflated) + y * log(mu) - mu - normc;
					}
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			lambda = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			mu = E * lambda;
			p_zeroinflated = 1.0 - pow(mu / (1.0 + mu), alpha);
			if (mu / size > cutoff) {
				/*
				 * NegativeBinomial 
				 */
				p = size / (size + mu);
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_negative_binomial_P((unsigned int) y, p, size);
			} else {
				/*
				 * The Poission limit 
				 */
				logll[i] = p_zeroinflated + (1.0 - p_zeroinflated) * gsl_cdf_poisson_P((unsigned int) y, mu);
			}
		}
	}

	for (i = 0; i < IABS(m); i++) {
		if (gsl_isinf(logll[i]))
			logll[i] = ((double) FLT_MAX) * gsl_isinf(logll[i]);
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_binomial(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Binomial(n, p)
	 */
	int i;

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int status;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx], p;

	/*
	 * this is a special case that should just return 0 or 1
	 */
	if (ISZERO(y) && ISZERO(n)) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				logll[i] = 0.0;		       /* log(1) = 0 */
			}
		} else {
			for (i = 0; i < -m; i++) {
				logll[i] = 1.0;
			}
		}
		return GMRFLib_SUCCESS;
	}

	/*
	 * this is the normal case...
	 */
	LINK_INIT;
	if (m > 0) {
		gsl_sf_result res;
		status = gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);
		assert(status == GSL_SUCCESS);
		for (i = 0; i < m; i++) {
			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			p = DMAX(0.0, DMIN(1.0, p));
			if (ISEQUAL(p, 1.0)) {
				/*
				 * this is ok if we get a 0*log(0) expression for the reminder 
				 */
				if (n == (int) y) {
					logll[i] = res.val + y * log(p);
				} else {
					logll[i] = -DBL_MAX;
				}
			} else if (ISZERO(p)) {
				/*
				 * this is ok if we get a 0*log(0) expression for the reminder 
				 */
				if ((int) y == 0) {
					logll[i] = res.val + (n - y) * log(1.0 - p);
				} else {
					logll[i] = -DBL_MAX;
				}
			} else {
				logll[i] = res.val + y * log(p) + (n - y) * log(1.0 - p);
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			p = PREDICTOR_INVERSE_LINK((x[i] + OFFSET(idx)));
			p = DMIN(1.0, p);
			logll[i] = gsl_cdf_binomial_P((unsigned int) y, p, (unsigned int) n);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_mix_gaussian(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * this is the wrapper for the gaussian_mix
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i, k, debug = 0;
	double *val = NULL, val_max, sum, prec, *xx = NULL, *ll = NULL, *storage = NULL, *points = NULL, *weights = NULL;

	Data_section_tp *ds = (Data_section_tp *) arg;
	prec = map_precision(ds->data_observations.mix_log_prec_gaussian[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	GMRFLib_ghq(&points, &weights, ds->mix_nq);	       /* these are just pointers... */
	storage = Calloc(3 * ds->mix_nq, double);	       /* use just one longer vector */
	val = storage;
	xx = storage + 1 * ds->mix_nq;
	ll = storage + 2 * ds->mix_nq;

	if (m > 0) {
		for (i = 0; i < m; i++) {
			if (debug) {
				printf("x[%1d] = %g prec = %g\n", i, x[i], prec);
			}
			for (k = 0; k < ds->mix_nq; k++) {
				xx[k] = x[i] + points[k] / sqrt(prec);
				if (debug) {
					printf("\txx[%1d] = %g\n", k, xx[k]);
				}
			}
			ds->mix_loglikelihood(ll, xx, ds->mix_nq, idx, x_vec, arg);

			for (k = 0; k < ds->mix_nq; k++) {
				if (debug) {
					printf("\tll[%1d] = %g integration.weight = %g\n", k, ll[k], weights[k]);
				}
				val[k] = log(weights[k]) + ll[k];
			}
			val_max = GMRFLib_max_value(val, ds->mix_nq, NULL);
			sum = 0.0;
			for (k = 0; k < ds->mix_nq; k++) {
				if (!ISNAN(val[k])) {
					sum += exp(val[k] - val_max);
				}
			}
			assert(sum > 0.0);
			logll[i] = log(sum) + val_max;
			if (debug) {
				printf("\nlogll[%1d] = %g\n", i, logll[i]);
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			for (k = 0; k < ds->mix_nq; k++) {
				xx[k] = x[i] + points[k] / sqrt(prec);
			}
			ds->mix_loglikelihood(ll, xx, -ds->mix_nq, idx, x_vec, arg);

			sum = 0.0;
			for (k = 0; k < ds->mix_nq; k++) {
				sum += weights[k] * ll[k];
			}
			logll[i] = sum;
		}
	}

	Free(storage);
	return GMRFLib_SUCCESS;
}
int loglikelihood_test_binomial_1(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Binomial(n, p) with (s,e) correction to prob for success
	 */
	int i;
#define SE(_p) (s * (_p) + (1.0 - (_p)) * (1.0 - e))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int status;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx], p,
	    s = map_probability(ds->data_observations.test_binomial_1_s[GMRFLib_thread_id][0], MAP_FORWARD, NULL),
	    e = map_probability(ds->data_observations.test_binomial_1_e[GMRFLib_thread_id][0], MAP_FORWARD, NULL);

	LINK_INIT;
	if (m > 0) {
		gsl_sf_result res;
		status = gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);
		assert(status == GSL_SUCCESS);
		for (i = 0; i < m; i++) {
			p = PREDICTOR_INVERSE_LINK(SE(x[i] + OFFSET(idx)));
			p = SE(p);
			if (p > 1.0) {
				/*
				 * need this for the link = "log" that was requested...
				 */
				logll[i] = res.val - SQR(DMIN(10.0, n)) * SQR(x[i] + OFFSET(idx) - (-5.0));
				// printf("idx x logl %d %g %g\n", idx, x[i], logll[i]);
			} else {
				if (ISEQUAL(p, 1.0)) {
					/*
					 * this is ok if we get a 0*log(0) expression for the reminder 
					 */
					if (n == (int) y) {
						logll[i] = res.val + y * log(p);
					} else {
						logll[i] = -DBL_MAX;
					}
				} else if (ISZERO(p)) {
					/*
					 * this is ok if we get a 0*log(0) expression for the reminder 
					 */
					if ((int) y == 0) {
						logll[i] = res.val + (n - y) * log(1.0 - p);
					} else {
						logll[i] = -DBL_MAX;
					}
				} else {
					logll[i] = res.val + y * log(p) + (n - y) * log(1.0 - p);
				}
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			p = PREDICTOR_INVERSE_LINK(SE((x[i] + OFFSET(idx))));
			p = SE(p);
			p = DMIN(1.0, p);
			logll[i] = gsl_cdf_binomial_P((unsigned int) y, p, (unsigned int) n);
		}
	}

	LINK_END;
#undef SE
	return GMRFLib_SUCCESS;
}
int loglikelihood_cbinomial(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * z ~ CBinomial(k, n, p) == Binomial(k, 1-(1-p)^n)
	 */
	int i;

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	int status;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], k = ds->data_observations.cbinomial_k[idx], n = ds->data_observations.cbinomial_n[idx], p;

	LINK_INIT;
	if (m > 0) {
		gsl_sf_result res;
		status = gsl_sf_lnchoose_e((unsigned int) k, (unsigned int) y, &res);	/* Yes, its 'k' */
		assert(status == GSL_SUCCESS);
		for (i = 0; i < m; i++) {
			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			p = 1.0 - pow(1.0 - p, n);
			p = DMIN(1.0, DMAX(0.0, p));
			if (ISEQUAL(p, 1.0)) {
				/*
				 * this is ok if we get a 0*log(0) expression for the reminder 
				 */
				if (k == (int) y) {
					logll[i] = res.val + y * log(p);
				} else {
					logll[i] = -DBL_MAX;
				}
			} else if (ISZERO(p)) {
				/*
				 * this is ok if we get a 0*log(0) expression for the reminder 
				 */
				if ((int) y == 0) {
					logll[i] = res.val + (k - y) * log(1.0 - p);
				} else {
					logll[i] = -DBL_MAX;
				}
			} else {
				logll[i] = res.val + y * log(p) + (k - y) * log(1.0 - p);
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			p = PREDICTOR_INVERSE_LINK((x[i] + OFFSET(idx)));
			p = 1.0 - pow(1.0 - p, n);
			p = DMIN(1.0, DMAX(0.0, p));
			logll[i] = gsl_cdf_binomial_P((unsigned int) y, p, (unsigned int) k);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_binomial0(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Binomial : y ~ p*1[y=0] + (1-p) Binomial(n, p | y > 0), where logit(p) = x. 
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx],
	    p = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), prob = 0.0;

	LINK_INIT;
	if ((int) y == 0) {
		/*
		 * this is just the point-mass at zero 
		 */
		if (m > 0) {
			for (i = 0; i < m; i++) {
				logll[i] = log(p);
			}
		} else {
			for (i = 0; i < -m; i++) {
				logll[i] = p;
			}
		}
	} else {
		gsl_sf_result res;
		gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);
		if (m > 0) {
			for (i = 0; i < m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - p) + res.val + y * log(prob) + (n - y) * log(1.0 - prob) - log(1.0 - pow(1.0 - prob, n));
			}
		} else {
			for (i = 0; i < -m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = p + (1.0 - p) * (gsl_cdf_binomial_P((unsigned int) y, prob, (unsigned int) n) -
							    gsl_cdf_binomial_P((unsigned int) 0, prob, (unsigned int) n));
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_binomial1(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Binomial : y ~ p*1[y=0] + (1-p)*Binomial(n, p), where logit(p) = x. 
	 */
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx],
	    p = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), prob = 0.0, logA, logB;

	gsl_sf_result res;
	gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);

	LINK_INIT;
	if ((int) y == 0) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logA = log(p);
				logB = log(1.0 - p) + res.val + y * log(prob) + (n - y) * log(1.0 - prob);
				// logll[i] = log(p + (1.0 - p) * gsl_ran_binomial_pdf((unsigned int) y, prob, (unsigned int) n));
				logll[i] = eval_logsum_safe(logA, logB);
			}
		} else {
			for (i = 0; i < -m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = p + (1.0 - p) * gsl_cdf_binomial_P((unsigned int) y, prob, (unsigned int) n);
			}
		}
	} else {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - p) + res.val + y * log(prob) + (n - y) * log(1.0 - prob);
			}
		} else {
			for (i = 0; i < -m; i++) {
				prob = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = p + (1.0 - p) * gsl_cdf_binomial_P((unsigned int) y, prob, (unsigned int) n);
			}
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_binomial2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated Binomial : y ~ prob*1[y=0] + (1-prob)*Binomial(n, p), where logit(p) = x, and prob = 1-p^alpha.
	 */
#define PROB(xx) (exp(xx)/(1.0+exp(xx)))
#define PROBZERO(xx) (1.0-pow(PROB(xx), alpha))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx], pzero, p,
	    alpha = map_exp(ds->data_observations.zeroinflated_alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), logA, logB;

	gsl_sf_result res;
	gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);

	LINK_INIT;
	if ((int) y == 0) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				pzero = PROBZERO(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (gsl_isinf(pzero) || gsl_isinf(p)) {
					logll[i] = -DBL_MAX;
				} else {
					if (ISZERO(pzero)) {
						logll[i] = res.val + y * log(p) + (n - y) * log(1.0 - p);
					} else {
						logA = log(pzero);
						logB = log(1.0 - pzero) + res.val + y * log(p) + (n - y) * log(1.0 - p);
						// logll[i] = log(pzero + (1.0 - pzero) * gsl_ran_binomial_pdf((unsigned int) y, p, (unsigned int) n));
						logll[i] = eval_logsum_safe(logA, logB);
					}
				}
			}
		} else {
			for (i = 0; i < -m; i++) {
				pzero = PROBZERO(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (gsl_isinf(pzero) || gsl_isinf(p)) {
					logll[i] = -DBL_MAX;
				} else {
					logll[i] = pzero + (1.0 - pzero) * gsl_cdf_binomial_P((unsigned int) y, p, (unsigned int) n);
				}
			}
		}
	} else {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				pzero = PROBZERO(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (gsl_isinf(pzero) || gsl_isinf(p)) {
					logll[i] = -DBL_MAX;
				} else {
					logll[i] = log(1.0 - pzero) + res.val + y * log(p) + (n - y) * log(1.0 - p);
				}
			}
		} else {
			for (i = 0; i < -m; i++) {
				pzero = PROBZERO(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (gsl_isinf(pzero) || gsl_isinf(p)) {
					logll[i] = -DBL_MAX;
				} else {
					logll[i] = pzero + (1.0 - pzero) * gsl_cdf_binomial_P((unsigned int) y, p, (unsigned int) n);
				}
			}
		}
	}

	LINK_END;
#undef PROB
#undef PROBZERO
	return GMRFLib_SUCCESS;
}
double eval_logsum_safe(double lA, double lB)
{
	/*
	 * evaluate log( exp(lA) + exp(lB) ) in a safe way 
	 */

	if (lA > lB) {
		return lA + log(1.0 + exp(lB - lA));
	} else {
		return lB + log(1.0 + exp(lA - lB));
	}
}
int loglikelihood_zero_n_inflated_binomial2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroNinflated Binomial : see doc from JS.
	 */
#define P1(xx) pow(exp(xx)/(1.0+exp(xx)), alpha1)
#define P2(xx) pow(1.0 - exp(xx)/(1.0+exp(xx)), alpha2)

	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], n = ds->data_observations.nb[idx],
	    alpha1 = map_exp(ds->data_observations.zero_n_inflated_alpha1_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL),
	    alpha2 = map_exp(ds->data_observations.zero_n_inflated_alpha2_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL), p, p1, p2, logA, logB;

	gsl_sf_result res;
	gsl_sf_lnchoose_e((unsigned int) n, (unsigned int) y, &res);

	LINK_INIT;
	if ((int) y == 0) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				p1 = P1(x[i] + OFFSET(idx));
				p2 = P2(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (ISINF(p1) || ISINF(p2) || ISINF(p)) {
					logll[i] = -DBL_MAX;
				} else {
					if (ISZERO(1.0 - p1)) {
						logll[i] = log(p2) + res.val + y * log(p) + (n - y) * log(1.0 - p);
					} else {
						logA = log((1.0 - p1)) + log(p2);
						logB = log(p1) + log(p2) + res.val + y * log(p) + (n - y) * log(1.0 - p);
						// logll[i] = log((1.0 - p1) * p2 + p1 * p2 * gsl_ran_binomial_pdf((unsigned int) y, p, (unsigned int) n));
						logll[i] = eval_logsum_safe(logA, logB);
					}
				}
			}
		} else {
			assert(0 == 1);
		}
	} else if ((int) y == (int) n) {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				p1 = P1(x[i] + OFFSET(idx));
				p2 = P2(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (ISINF(p1) || ISINF(p2) || ISINF(p)) {
					logll[i] = -DBL_MAX;
				} else {
					if (ISZERO(1.0 - p2)) {
						logll[i] = log(p1) + res.val + y * log(p) + (n - y) * log(1.0 - p);
					} else {
						logA = log((1.0 - p2)) + log(p1);
						logB = log(p1) + log(p2) + res.val + y * log(p) + (n - y) * log(1.0 - p);
						// logll[i] = log((1.0 - p2) * p1 + p1 * p2 * gsl_ran_binomial_pdf((unsigned int) y, p, (unsigned int) n));
						logll[i] = eval_logsum_safe(logA, logB);
					}
				}
			}
		} else {
			assert(0 == 1);
		}
	} else {
		if (m > 0) {
			for (i = 0; i < m; i++) {
				p1 = P1(x[i] + OFFSET(idx));
				p2 = P2(x[i] + OFFSET(idx));
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				if (ISINF(p1) || ISINF(p2) || ISINF(p)) {
					logll[i] = -DBL_MAX;
				} else {
					logll[i] = log(p1) + log(p2) + res.val + y * log(p) + (n - y) * log(1.0 - p);
				}
			}
		} else {
			assert(0 == 1);
		}
	}

	LINK_END;
#undef P1
#undef P2
	return GMRFLib_SUCCESS;
}
int loglikelihood_gamma(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * Gamma
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx];
	double s = ds->data_observations.gamma_weight[idx];
	double phi_param = map_exp(ds->data_observations.gamma_log_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double phi, mu, a, b, c;

	phi = phi_param * s;
	c = -gsl_sf_lngamma(phi) + (phi - 1.0) * log(y) + phi * log(phi);
	LINK_INIT;

	if (m > 0) {
		for (i = 0; i < m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = c - phi * (log(mu) + y / mu);
		}
	} else {
		for (i = 0; i < -m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = phi;
			b = mu / phi;
			logll[i] = gsl_cdf_gamma_P(y, a, b);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_gammacount(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * Gammacount
	 */
#define G(_alpha, _beta) gsl_cdf_gamma_P((_beta), (_alpha), 1.0)


	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx];
	double alpha = map_exp(ds->data_observations.gammacount_log_alpha[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double beta, mu, p, logp;

	LINK_INIT;

	if (m > 0) {
		for (i = 0; i < m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			beta = alpha * mu;
			p = G(y * alpha, beta) - G((y + 1.0) * alpha, beta);
			logp = log(p);
			// this can go in over/underflow...
			if (ISINF(logp) || ISNAN(logp)) {
				logll[i] = log(GMRFLib_eps(1.0)) + PENALTY * SQR(x[i] + OFFSET(idx));
			} else {
				logll[i] = logp;
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			beta = alpha * mu;
			logll[i] = G((y + 1.0) * alpha, beta);
		}
	}

	LINK_END;
#undef G
	return GMRFLib_SUCCESS;
}
int loglikelihood_beta(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * Beta : y ~ Beta(y; a, b) = BetaFunction(a,b)^{-1} y^{a-1} (1-y)^{b-1}. mu = a/(a+b), phi = a+b = exp(theta).
	 */

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx];
	double phi = map_exp(ds->data_observations.beta_precision_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double a, b, mu;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = mu * phi;
			b = -mu * phi + phi;
			logll[i] = -gsl_sf_lnbeta(a, b) + (a - 1.0) * log(y) + (b - 1.0) * log(1.0 - y);
		}
	} else {
		for (i = 0; i < -m; i++) {
			mu = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = mu * phi;
			b = -mu * phi + phi;
			logll[i] = gsl_cdf_beta_P(y, a, b);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_betabinomial(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * BetaBinomial : y ~ BetaBinomial(n, a, b), where logit(p) = a/(a+b), overdispertsion = 1/(a+b+1)
	 */
#define LOGGAMMA_INT(xx) gsl_sf_lnfact((unsigned int) ((xx) - 1))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	int y = (int) ds->data_observations.y[idx];
	int n = (int) ds->data_observations.nb[idx];

	double rho = map_probability(ds->data_observations.betabinomial_overdispersion_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double p, a, b;
	double normc = LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1);

	LINK_INIT;
	if (m > 0) {
		// issues occur when x[i] is to large
		double p_upper = 0.999, xmax;
		xmax = GMRFLib_max_value(x, m, NULL) + OFFSET(idx);
		p = PREDICTOR_INVERSE_LINK(xmax);
		if (p < p_upper) {
			for (i = 0; i < m; i++) {
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				a = p * (1.0 - rho) / rho;
				b = (p * rho - p - rho + 1.0) / rho;
				logll[i] = normc + gsl_sf_lnbeta(y + a, n - y + b) - gsl_sf_lnbeta(a, b);
			}
		} else {
			// extrapolate linearly
			double xx[3], ll[3], h = 1.0E-4, diff, ddiff, dx;
			xx[1] = PREDICTOR_LINK(p_upper);
			xx[0] = xx[1] - h;
			xx[2] = xx[1] + h;
			for (i = 0; i < 3; i++) {
				p = PREDICTOR_INVERSE_LINK(xx[i]);
				a = p * (1.0 - rho) / rho;
				b = (p * rho - p - rho + 1.0) / rho;
				ll[i] = normc + gsl_sf_lnbeta(y + a, n - y + b) - gsl_sf_lnbeta(a, b);
			}
			diff = (ll[2] - ll[0]) / (2.0 * h);
			ddiff = (ll[2] - 2.0 * ll[1] + ll[0]) / SQR(h);
			diff = DMIN(0.0, diff);		       /* must have */
			ddiff = DMIN(0.0, ddiff);	       /* must have */
			for (i = 0; i < m; i++) {
				dx = (x[i] + OFFSET(idx)) - xx[1];
				logll[i] = ll[1] + dx * diff + 0.5 * SQR(dx) * ddiff;
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			/*
			 * if 'n' is ``large'', then we need to do something approximate here... I guess we get the question back if so is the case. So we issue a
			 * warning for the ``extreme case'' only.
			 */
			static char give_warning = 1;
			if (n > 500 && give_warning) {
				give_warning = 0;
				printf("\n*** Warning ***  Version [%s]", RCSId);
				printf("\n*** Warning ***  The PIT calculations for the BetaBinomial can be time-consuming when Ntrials is large.");
				printf("\n*** Warning ***  Please contact <help@r-inla.org> if this becomes an issue.\n");
			}

			int yy;
			double normc2;

			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = p * (1.0 - rho) / rho;
			b = (p * rho - p - rho + 1.0) / rho;
			normc2 = LOGGAMMA_INT(n + 1) - gsl_sf_lnbeta(a, b);
			logll[i] = 0.0;

			if (y <= n / 2) {		       /* integer arithmetic is ok */
				for (yy = y; yy >= 0; yy--) {
					logll[i] += exp(normc2 - LOGGAMMA_INT(yy + 1) - LOGGAMMA_INT(n - yy + 1) + gsl_sf_lnbeta(yy + a, n - yy + b));
				}
			} else {
				for (yy = y + 1; yy <= n; yy++) {
					logll[i] += exp(normc2 - LOGGAMMA_INT(yy + 1) - LOGGAMMA_INT(n - yy + 1) + gsl_sf_lnbeta(yy + a, n - yy + b));
				}
				logll[i] = 1.0 - logll[i];
			}
		}
	}

	LINK_END;
#undef LOGGAMMA_INT
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_betabinomial0(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated BetaBinomial : y ~ BetaBinomial(n, a, b), where logit(p) = a/(a+b), overdispertsion = 1/(a+b+1)
	 */
#define LOGGAMMA_INT(xx) gsl_sf_lnfact((unsigned int) ((xx) - 1))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i, yzero = 0;
	Data_section_tp *ds = (Data_section_tp *) arg;
	int y = (int) ds->data_observations.y[idx];
	int n = (int) ds->data_observations.nb[idx];

	double rho = map_probability(ds->data_observations.zeroinflated_rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double pzero = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double p, a, b, prob_zero;
	double normc = LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1);
	double normc_zero = LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(yzero + 1) - LOGGAMMA_INT(n - yzero + 1);

	LINK_INIT;
	if (m > 0) {
		if (y == 0) {
			for (i = 0; i < m; i++) {
				logll[i] = log(pzero);
			}
		} else {
			for (i = 0; i < m; i++) {
				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				a = p * (1.0 - rho) / rho;
				b = (p * rho - p - rho + 1.0) / rho;
				prob_zero = exp(normc_zero + gsl_sf_lnbeta(yzero + a, n - yzero + b) - gsl_sf_lnbeta(a, b));
				logll[i] = log(1.0 - pzero) + normc + gsl_sf_lnbeta(y + a, n - y + b) - gsl_sf_lnbeta(a, b) - log(1.0 - prob_zero);
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			if (y == 0) {
				logll[i] = pzero;
			} else {
				int yy;
				double normc2;

				p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				a = p * (1.0 - rho) / rho;
				b = (p * rho - p - rho + 1.0) / rho;
				normc2 = LOGGAMMA_INT(n + 1) - gsl_sf_lnbeta(a, b);
				prob_zero = normc + gsl_sf_lnbeta(yzero + a, n - yzero + b) - gsl_sf_lnbeta(a, b);
				logll[i] = 0.0;
				for (yy = y; yy > 0; yy--) {
					logll[i] += exp(normc2 - LOGGAMMA_INT(yy + 1) - LOGGAMMA_INT(n - yy + 1) + gsl_sf_lnbeta(yy + a, n - yy + b));
				}
				logll[i] = pzero + (1.0 - pzero) * logll[i] / (1.0 - prob_zero);
			}
		}
	}

	LINK_END;
#undef LOGGAMMA_INT
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_betabinomial1(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated BetaBinomial : y ~ BetaBinomial(n, a, b), where logit(p) = a/(a+b), overdispertsion = 1/(a+b+1)
	 */
#define LOGGAMMA_INT(xx) gsl_sf_lnfact((unsigned int) ((xx) - 1))

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	int y = (int) ds->data_observations.y[idx];
	int n = (int) ds->data_observations.nb[idx];

	double rho = map_probability(ds->data_observations.zeroinflated_rho_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double pzero = map_probability(ds->data_observations.prob_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double p, a, b, tmp;
	double normc = LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1);

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = p * (1.0 - rho) / rho;
			b = (p * rho - p - rho + 1.0) / rho;
			tmp = log(1.0 - pzero) + normc + gsl_sf_lnbeta(y + a, n - y + b) - gsl_sf_lnbeta(a, b);
			if (y == 0) {
				logll[i] = GMRFLib_log_apbex(pzero, tmp);
			} else {
				logll[i] = tmp;
			}
		}
	} else {
		for (i = 0; i < -m; i++) {
			int yy;
			double normc2;

			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			a = p * (1.0 - rho) / rho;
			b = (p * rho - p - rho + 1.0) / rho;
			normc2 = LOGGAMMA_INT(n + 1) - gsl_sf_lnbeta(a, b);
			logll[i] = 0.0;
			for (yy = y; yy >= 0; yy--) {
				logll[i] += exp(normc2 - LOGGAMMA_INT(yy + 1) - LOGGAMMA_INT(n - yy + 1) + gsl_sf_lnbeta(yy + a, n - yy + b));
			}
			logll[i] = pzero + (1.0 - pzero) * logll[i];
		}
	}

	LINK_END;
#undef LOGGAMMA_INT
	return GMRFLib_SUCCESS;
}
int loglikelihood_zeroinflated_betabinomial2(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * zeroinflated BetaBinomial : y ~ prob*1[y=0] + (1-prob)*BetaBinomial(n, p, delta), where logit(p) = x, and prob = 1-p^alpha.
	 */
#define PROB(xx)         (exp(xx)/(1.0+exp(xx)))
#define PROBZERO(xx)     (1.0-pow(PROB(xx), alpha))
#define LOGGAMMA(xx)     gsl_sf_lngamma(xx)
#define LOGGAMMA_INT(xx) gsl_sf_lnfact((unsigned int) ((xx) - 1))

	if (m == 0) {
		return 0;
	}

	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	int y = (int) ds->data_observations.y[idx];
	int n = (int) ds->data_observations.nb[idx];
	double pzero, p;
	double alpha = map_exp(ds->data_observations.zeroinflated_alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double delta = map_exp(ds->data_observations.zeroinflated_delta_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double logA, logB;

	LINK_INIT;
	if ((int) y == 0) {
		for (i = 0; i < m; i++) {
			pzero = PROBZERO(x[i] + OFFSET(idx));
			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			if (gsl_isinf(pzero) || gsl_isinf(p)) {
				logll[i] = -DBL_MAX;
			} else {
				// logll[i] = log(pzero + (1.0 - pzero) * exp(LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1)
				// + LOGGAMMA(delta * p + y) + LOGGAMMA(n + delta * (1.0 - p) - y) - LOGGAMMA(delta + n)
				// + LOGGAMMA(delta) - LOGGAMMA(delta * p) - LOGGAMMA(delta * (1.0 - p))));

				logA = log(pzero);
				logB = log(1.0 - pzero) + (LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1)
							   + LOGGAMMA(delta * p + y) + LOGGAMMA(n + delta * (1.0 - p) - y) - LOGGAMMA(delta + n)
							   + LOGGAMMA(delta) - LOGGAMMA(delta * p) - LOGGAMMA(delta * (1.0 - p)));
				logll[i] = eval_logsum_safe(logA, logB);
			}
		}
	} else {
		for (i = 0; i < m; i++) {
			pzero = PROBZERO(x[i] + OFFSET(idx));
			p = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			if (gsl_isinf(pzero) || gsl_isinf(p)) {
				logll[i] = -DBL_MAX;
			} else {
				logll[i] = log(1.0 - pzero) + (LOGGAMMA_INT(n + 1) - LOGGAMMA_INT(y + 1) - LOGGAMMA_INT(n - y + 1)
							       + LOGGAMMA(delta * p + y) + LOGGAMMA(n + delta * (1.0 - p) - y) - LOGGAMMA(delta + n)
							       + LOGGAMMA(delta) - LOGGAMMA(delta * p) - LOGGAMMA(delta * (1.0 - p)));
			}
		}
	}

	LINK_END;
#undef PROB
#undef PROBZERO
#undef LOGGAMMA
#undef LOGGAMMA_INT

	return GMRFLib_SUCCESS;
}
int loglikelihood_exp(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Exponential
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, ievent;
	double y, event, truncation, lower, upper, gama;

	y = ds->data_observations.y[idx];
	event = ds->data_observations.event[idx];
	ievent = (int) event;
	truncation = ds->data_observations.truncation[idx];
	lower = ds->data_observations.lower[idx];
	upper = ds->data_observations.upper[idx];

	LINK_INIT;
	if (m > 0) {
		switch (ievent) {
		case SURV_EVENT_FAILURE:
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(gama) - gama * (y - truncation);
			}
			break;

		case SURV_EVENT_RIGHT:
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = -gama * (lower - truncation);
			}
			break;

		case SURV_EVENT_LEFT:
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - exp(-gama * (upper - truncation)));
			}
			break;

		case SURV_EVENT_INTERVAL:
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = -gama * (lower - truncation) + log(1.0 - exp(-gama * (upper - lower)));
			}
			break;
		default:
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
		}
		return GMRFLib_SUCCESS;
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_weibull(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ Weibull.
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, ievent;
	double y, event, truncation, lower, upper, alpha, gama, ypow, lowerpow, upperpow, truncationpow;

	y = ds->data_observations.y[idx];
	event = ds->data_observations.event[idx];
	ievent = (int) event;
	truncation = ds->data_observations.truncation[idx];
	lower = ds->data_observations.lower[idx];
	upper = ds->data_observations.upper[idx];
	alpha = map_alpha_weibull(ds->data_observations.alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	truncationpow = pow(truncation, alpha);

	LINK_INIT;
	if (m > 0) {
		switch (ievent) {
		case SURV_EVENT_FAILURE:
			ypow = pow(y, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(gama) + log(alpha) + (alpha - 1.0) * log(y) - gama * (ypow - truncationpow);
			}
			break;

		case SURV_EVENT_RIGHT:
			lowerpow = pow(lower, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = -gama * (lowerpow - truncationpow);
			}
			break;

		case SURV_EVENT_LEFT:
			upperpow = pow(upper, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - exp(-gama * (upperpow - truncationpow)));
			}
			break;

		case SURV_EVENT_INTERVAL:
			lowerpow = pow(lower, alpha);
			upperpow = pow(upper, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = -gama * (lowerpow - truncationpow) + log(1.0 - exp(-gama * (upperpow - lowerpow)));
			}
			break;
		default:
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
		}
		return GMRFLib_SUCCESS;
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_loglogistic(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ LOGLOGISTIC. cdf is 1/(1 + (y/eta)^-alpha); see http://en.wikipedia.org/wiki/Log-logistic_distribution
	 */
#define logf(_y, _eta) (log(alpha/(_eta)) + (alpha-1.0)*log((_y)/(_eta)) - 2.0*log(1.0 + pow((_y)/(_eta), alpha)))
#define F(_y, _eta) ((_y) <= 0.0 ? 0.0 : (1.0/(1.0 + pow((_y)/(_eta), -alpha))))

#define logff(_y, _eta) (logf(_y, _eta) - log(1.0 - F(truncation, _eta)))
#define FF(_y, _eta)  ((F(_y, _eta) -F(truncation, _eta))/(1.0 - F(truncation, _eta)))

	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, ievent;
	double y, event, truncation, lower, upper, alpha, eta;

	y = ds->data_observations.y[idx];
	event = ds->data_observations.event[idx];
	ievent = (int) event;
	truncation = ds->data_observations.truncation[idx];
	lower = ds->data_observations.lower[idx];
	upper = ds->data_observations.upper[idx];
	alpha = map_alpha_loglogistic(ds->data_observations.alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	LINK_INIT;

	if (m > 0) {
		switch (ievent) {
		case SURV_EVENT_FAILURE:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = logff(y, eta);
			}
			break;

		case SURV_EVENT_RIGHT:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - FF(upper, eta));
			}
			break;

		case SURV_EVENT_LEFT:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(FF(lower, eta));
			}
			break;

		case SURV_EVENT_INTERVAL:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(FF(upper, eta) - FF(lower, eta));
			}
			break;
		default:
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
		}
		return GMRFLib_SUCCESS;
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	LINK_END;
#undef logf
#undef F
#undef logff
#undef FF
	return GMRFLib_SUCCESS;
}
int loglikelihood_lognormal(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ LOGNORMAL
	 */
#define logf(_y, _eta) (LOG_NORMC_GAUSSIAN -log(_y) + 0.5*lprec - 0.5*prec*SQR( log(_y) - (_eta) ))
#define F(_y, _eta) ((_y) <= 0.0 ? 0.0 : inla_Phi((log(_y)-(_eta))*sprec))

#define logff(_y, _eta) (logf(_y, _eta) - log(1.0 - F(truncation, _eta)))
#define FF(_y, _eta)  ((F(_y, _eta) - F(truncation, _eta))/(1.0 - F(truncation, _eta)))

	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, ievent;
	double y, event, truncation, lower, upper, eta, lprec, prec, sprec;

	y = ds->data_observations.y[idx];
	event = ds->data_observations.event[idx];
	ievent = (int) event;
	truncation = ds->data_observations.truncation[idx];
	lower = ds->data_observations.lower[idx];
	upper = ds->data_observations.upper[idx];
	lprec = ds->data_observations.log_prec_gaussian[GMRFLib_thread_id][0];
	prec = map_precision(ds->data_observations.log_prec_gaussian[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	sprec = sqrt(prec);
	LINK_INIT;

	if (m > 0) {
		switch (ievent) {
		case SURV_EVENT_FAILURE:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = logff(y, eta);
			}
			break;

		case SURV_EVENT_RIGHT:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(1.0 - FF(upper, eta));
			}
			break;

		case SURV_EVENT_LEFT:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(FF(lower, eta));
			}
			break;
		case SURV_EVENT_INTERVAL:
			for (i = 0; i < m; i++) {
				eta = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(FF(upper, eta) - FF(lower, eta));
			}
			break;

		default:
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
		}
		return GMRFLib_SUCCESS;
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	LINK_END;
#undef logf
#undef F
#undef logff
#undef FF
	return GMRFLib_SUCCESS;
}
int loglikelihood_weibull_cure(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * Likelihood model for Patrick's and Silvia's model. (Internal use only.)
	 *
	 * y ~ Weibull with unknown entry point and 'zero-inflation'; see note from Patrick.B.
	 *
	 * event can be FAILURE, LEFT, RIGHT or INTERVAL
	 */

	if (m == 0) {
		return GMRFLib_SUCCESS;
	}

	Data_section_tp *ds = (Data_section_tp *) arg;
	int i, ievent;
	double y, event, truncation, lower, upper, alpha, gama, ypow, lowerpow, upperpow, truncationpow, p, onemp, podds;

	y = ds->data_observations.y[idx];
	event = ds->data_observations.event[idx];
	ievent = (int) event;
	truncation = ds->data_observations.truncation[idx];
	lower = ds->data_observations.lower[idx];
	upper = ds->data_observations.upper[idx];
	alpha = map_alpha_weibull_cure(ds->data_observations.alpha_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	p = map_p_weibull_cure(ds->data_observations.p_intern[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	truncationpow = pow(truncation, alpha);
	onemp = 1.0 - p;
	podds = p / onemp;
	LINK_INIT;


	if (m > 0) {
		switch (ievent) {
		case SURV_EVENT_FAILURE:
			ypow = pow(y, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(gama) + log(alpha) + (alpha - 1.0) * log(y) - gama * ypow - log(podds + exp(-gama * truncationpow)
				    );
			}
			break;

		case SURV_EVENT_RIGHT:
			lowerpow = pow(lower, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(p + onemp * exp(-gama * lowerpow)) - log(p + onemp * exp(-gama * truncationpow));
			}
			break;

		case SURV_EVENT_LEFT:
			upperpow = pow(upper, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(onemp) - gama * (upperpow - truncationpow) - log(1.0 - onemp * exp(-gama * truncationpow));
			}
			break;

		case SURV_EVENT_INTERVAL:
			lowerpow = pow(lower, alpha);
			upperpow = pow(upper, alpha);
			for (i = 0; i < m; i++) {
				gama = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
				logll[i] = log(onemp) - gama * (upperpow - lowerpow) - log(1.0 - onemp * exp(-gama * truncationpow));
			}
			break;
		default:
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
		}
		return GMRFLib_SUCCESS;
	} else {
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}
	LINK_END;
	return GMRFLib_SUCCESS;
}

int loglikelihood_stochvol(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y ~ N(0, var = exp(x) + 1/tau) 
	 */
	int i;

	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	Data_section_tp *ds = (Data_section_tp *) arg;
	double y = ds->data_observations.y[idx], var;
	double tau = map_precision(ds->data_observations.log_offset_prec[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	double var_offset;

	LINK_INIT;
	var_offset = ((ISINF(tau) || ISNAN(tau)) ? 0.0 : 1.0 / tau);
	if (m > 0) {
		for (i = 0; i < m; i++) {
			var = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx)) + var_offset;
			logll[i] = LOG_NORMC_GAUSSIAN - 0.5 * log(var) - 0.5 * SQR(y) / var;
		}
	} else {
		for (i = 0; i < -m; i++) {
			var = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx)) + var_offset;
			logll[i] = 1.0 - 2.0 * (1.0 - inla_Phi(ABS(y) / sqrt(var)));
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_stochvol_t(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y / exp(x/2)  ~ Student-t_dof(0, ***var = 1***)
	 *
	 * Note that Student-t_dof has variance dof/(dof-2), so we need to scale it.
	 */
	int i;
	if (m == 0) {
		return GMRFLib_LOGL_COMPUTE_CDF;
	}
	Data_section_tp *ds = (Data_section_tp *) arg;
	double dof, y, sd, sd2, obs, var_u;

	dof = map_dof(ds->data_observations.dof_intern_svt[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	y = ds->data_observations.y[idx];
	sd2 = dof / (dof - 2.0);
	sd = sqrt(sd2);
	LINK_INIT;
	if (m > 0) {
		double lg1, lg2, f;

		lg1 = gsl_sf_lngamma(dof / 2.0);
		lg2 = gsl_sf_lngamma((dof + 1.0) / 2.0);
		for (i = 0; i < m; i++) {
			var_u = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			f = sqrt(var_u) / sd;
			obs = y / f;
			logll[i] = lg2 - lg1 - 0.5 * log(M_PI * dof) - (dof + 1.0) / 2.0 * log(1.0 + SQR(obs) / dof) - log(f);
		}
	} else {
		for (i = 0; i < -m; i++) {
			var_u = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			logll[i] = 1.0 - 2.0 * gsl_cdf_tdist_Q(ABS(y) * sd / sqrt(var_u), dof);
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int loglikelihood_stochvol_nig(double *logll, double *x, int m, int idx, double *x_vec, void *arg)
{
	/*
	 * y / exp(x/2)  ~ NIG with skew and shape parameter. beta = skew, psi = shape. Note that E=1 and Var=1.
	 *
	 *
	 * density: gamma
	 *          * exp[ psi^2 + beta*(gamma*x + beta) ]
	 *          * K_1[sqrt(beta^2+psi^2)*sqrt((gamma*x+beta)^2 + psi^2)]
	 *
	 */
	if (m == 0) {
		return GMRFLib_SUCCESS;
	}
	int i;
	Data_section_tp *ds = (Data_section_tp *) arg;
	double skew, skew2, shape, shape2, y, gam, gam2, tmp, obs, a, var_u;

	skew = ds->data_observations.skew_intern_svnig[GMRFLib_thread_id][0];
	skew2 = SQR(skew);
	shape = map_shape_svnig(ds->data_observations.shape_intern_svnig[GMRFLib_thread_id][0], MAP_FORWARD, NULL);
	shape2 = SQR(shape);
	gam2 = 1.0 + SQR(skew) / SQR(shape);
	gam = sqrt(gam2);
	y = ds->data_observations.y[idx];
	a = log(gam * shape / M_PI) + 0.5 * log(skew2 + shape2) + shape2;

	LINK_INIT;
	if (m > 0) {
		for (i = 0; i < m; i++) {
			var_u = PREDICTOR_INVERSE_LINK(x[i] + OFFSET(idx));
			obs = y / sqrt(var_u);
			tmp = SQR(gam * obs + skew) + shape2;
			logll[i] = a - 0.5 * log(tmp) + skew * (gam * obs + skew)
			    + gsl_sf_bessel_lnKnu(1.0, sqrt((skew2 + shape2) * tmp)) - log(var_u) / 2.0;
		}
	}

	LINK_END;
	return GMRFLib_SUCCESS;
}
int inla_sread_colon_ints(int *i, int *j, const char *str)
{
	/*
	 * read integer I and J from STR using format I:J
	 */
	return (sscanf(str, "%d:%d", i, j) == 2 ? INLA_OK : INLA_FAIL);
}
int inla_sread(void *x, int nx, const char *str, int code)
{
	/*
	 * code = 0: int. code = 1: double 
	 */

	char *strtok_ptr = NULL, *token, *p;
	const char *delim = " \t";
	double *dx = (double *) x;
	int *ix = (int *) x;
	int count = 0;
	int debug = 0;
	int ok;

	if (debug)
		printf("read %d entries from %s\n", nx, str);

	assert(code == 0 || code == 1);
	p = GMRFLib_strdup(str);
	while ((token = GMRFLib_strtok_r(p, delim, &strtok_ptr))) {
		p = NULL;
		ok = 1;
		if (debug) {
			printf("strip [%s] into [%s]\n", str, token);
		}

		if (code == 0) {
			if (sscanf(token, "%d", &ix[count]) == 0) {
				ok = 0;
			}
		} else if (code == 1) {
			if (sscanf(token, "%lf", &dx[count]) == 0) {
				ok = 0;
			}
		}
		if (ok)
			count++;

		if (count == nx) {
			break;
		}
	}

	if (count != nx) {
		return INLA_FAIL;
	}
	Free(p);

	return INLA_OK;
}
int inla_sread_q(void **x, int *nx, const char *str, int code)
{
	/*
	 * code = 0: int. code = 1: double
	 * 
	 * this return the number of `ints' read in nx and in x 
	 */

	char *strtok_ptr = NULL, *token, *p;
	const char *delim = " \t";
	double *dx = NULL;
	double dx_try;
	int *ix = NULL;
	int count = 0;
	int debug = 0;
	int ix_try;
	int ok;

	assert(code == 0 || code == 1);
	p = GMRFLib_strdup(str);

	while ((token = GMRFLib_strtok_r(p, delim, &strtok_ptr))) {
		p = NULL;
		ok = 0;
		if (debug) {
			printf("strip [%s] into [%s]\n", str, token);
		}
		if (code == 0) {
			if (sscanf(token, "%d", &ix_try) == 1)
				ok = 1;
		} else {
			if (sscanf(token, "%lf", &dx_try) == 1)
				ok = 1;
		}
		if (ok) {
			if (code == 0) {
				ix = Realloc(ix, count + 1, int);
				ix[count++] = ix_try;
			} else {
				dx = Realloc(dx, count + 1, double);
				dx[count++] = dx_try;
			}
		}
	}

	*nx = count;
	if (count == 0) {
		*x = NULL;
	} else {
		if (code == 0) {
			*x = (void *) ix;
		} else {
			*x = (void *) dx;
		}
	}

	if (debug) {
		int i;
		for (i = 0; i < *nx; i++) {
			if (code == 0) {
				printf("%s : %d %d\n", str, i, ix[i]);
			} else {
				printf("%s : %d %g\n", str, i, dx[i]);
			}
		}
	}

	Free(p);

	return INLA_OK;
}
int inla_sread_ints(int *x, int nx, const char *str)
{
	// read a fixed number of ints from str
	return inla_sread((void *) x, nx, str, 0);
}
int inla_sread_doubles(double *x, int nx, const char *str)
{
	// read a fixed number of doubles from str
	return inla_sread((void *) x, nx, str, 1);
}
int inla_sread_ints_q(int **x, int *nx, const char *str)
{
	// read an unknown number of ints from str
	return inla_sread_q((void **) x, nx, str, 0);
}
int inla_sread_doubles_q(double **x, int *nx, const char *str)
{
	// read an unknown number of doubles from str
	return inla_sread_q((void **) x, nx, str, 1);
}
int inla_is_NAs(int nx, const char *string)
{
	/*
	 * return GMRFLib_SUCCESS is string consists of nx NA's + whitespace separation 
	 */
	char *scopy, *p;
	const char *sep = " \t", *NA = "NA";
	int k = 0, nna = 0, debug = 0;

	if (debug) {
		printf("call inla_is_NAs: nx %d string %s\n", nx, string);
	}

	if (!string && nx)
		return !GMRFLib_SUCCESS;
	if (!string && !nx)
		return GMRFLib_SUCCESS;

	scopy = GMRFLib_strdup(string);
	p = strtok(scopy, sep);
	nna += (p && !strcmp(p, NA));

	if (debug)
		printf("get token %d : %s\n", k++, p);

	while (p) {
		p = strtok(NULL, sep);
		nna += (p && !strcmp(p, NA));

		if (debug)
			printf("get token %d : %s\n", k++, p);
	}

	Free(scopy);
	return (nna == nx ? GMRFLib_SUCCESS : !GMRFLib_SUCCESS);
}
const char *inla_string_join(const char *a, const char *b)
{
	/*
	 * join strings A and B into A:B. this returns a ptr to a static storage; be aware! 
	 */
	static char ans[1025];

	assert((a ? strlen(a) + 1 : 0) + (b ? strlen(b) + 1 : 0) < 1025);
	sprintf(ans, "%s%c%s", (a ? a : ""), INIPARSER_SEP, (b ? b : ""));
	return ans;
}
int inla_error_missing_required_field(const char *funcname, const char *secname, const char *field)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: section [%s]: missing required field [%s]\n\n", funcname, secname, field);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_open_file(const char *msg)
{
	fprintf(stderr, "\n\n*** ERROR *** \tfail to open file[%s] for writing. Exit...\n\n", msg);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_general(const char *msg)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s\n\n", msg);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_general2(const char *msg, const char *msg2)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: %s\n\n", msg, msg2);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_field_is_void(const char *funcname, const char *secname, const char *field, const char *value)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: section [%s]: field [%s] is void: [%s]\n\n", funcname, secname, field, value);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_file_numelm(const char *funcname, const char *filename, int n, int idiv)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: file [%s] contains [%1d] elements, which is not a multiple of [%1d]\n\n", funcname, filename, n, idiv);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_file_totnumelm(const char *funcname, const char *filename, int n, int total)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: file [%s] contains [%1d] elements, which is different from [n] = [%1d]\n\n", funcname, filename, n, total);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_file_error_sorted(const char *funcname, const char *filename, int n, int element_number, double val)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: file [%s] contains [%1d] elements, but element [%1d] = [%g] is void. The 'values' needs to be increasing!\n\n",
		funcname, filename, n, element_number, val);
	abort();
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_file_error(const char *funcname, const char *filename, int n, int element_number, double val)
{
	fprintf(stderr, "\n\n*** ERROR *** \t%s: file [%s] contains [%1d] elements, but element [%1d] = [%g] is void.\n\n",
		funcname, filename, n, element_number, val);
	abort();
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_error_file_error2(const char *funcname, const char *filename, int n, int element_number, double val, int element_number2, double val2)
{
	fprintf(stderr,
		"\n\n*** ERROR *** \t%s: file [%s] contains [%1d] elements, but element [%1d] = [%g] or [%1d] = [%g] is void.\n\n",
		funcname, filename, n, element_number, val, element_number2, val2);
	exit(EXIT_FAILURE);
	return INLA_OK;
}
int inla_read_fileinfo(inla_tp * mb, dictionary * ini, int sec, File_tp * file, const char *FILENAME)
{
	char *tag = (FILENAME ? GMRFLib_strdup(FILENAME) : GMRFLib_strdup("FILENAME"));
	char *secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));

	file->name = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, tag), NULL));

	/*
	 * Only signal error if FILENAME is NULL. 
	 */
	if (!file->name && !FILENAME) {
		inla_error_missing_required_field(__GMRFLib_FuncName, secname, tag);
	}
	if (mb->verbose) {
		printf("\t\tfile->name=[%s]\n", file->name);
	}
	Free(tag);

	return INLA_OK;
}
int inla_read_weightsinfo(inla_tp * mb, dictionary * ini, int sec, File_tp * file)
{
	char *secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));

	file->name = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FILENAME"), NULL));
	if (!file->name) {
		inla_error_missing_required_field(__GMRFLib_FuncName, secname, "filename");
	}
	if (mb->verbose) {
		printf("\t\tfile->name=[%s]\n", file->name);
	}
	return INLA_OK;
}
int inla_read_prior(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "PRIOR", "PARAMETERS", "FROM.THETA", "TO.THETA", default_prior);
}
int inla_read_prior_mix(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "MIX.PRIOR", "MIX.PARAMETERS", "MIX.FROM.THETA", "MIX.TO.THETA", default_prior);
}
int inla_read_prior_link(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "LINK.PRIOR", "LINK.PARAMETERS", "LINK.FROM.THETA", "LINK.TO.THETA", default_prior);
}
int inla_read_prior_link0(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "LINK.PRIOR0", "LINK.PARAMETERS0", "LINK.FROM.THETA0", "LINK.TO.THETA0", default_prior);
}
int inla_read_prior_link1(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "LINK.PRIOR1", "LINK.PARAMETERS1", "LINK.FROM.THETA1", "LINK.TO.THETA1", default_prior);
}
int inla_read_prior_link2(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "LINK.PRIOR2", "LINK.PARAMETERS2", "LINK.FROM.THETA2", "LINK.TO.THETA2", default_prior);
}
int inla_read_prior_group(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "GROUP.PRIOR", "GROUP.PARAMETERS", "GROUP.FROM.THETA", "GROUP.TO.THETA", default_prior);
}
int inla_read_prior_group0(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "GROUP.PRIOR0", "GROUP.PARAMETERS0", "GROUP.FROM.THETA0", "GROUP.TO.THETA0", default_prior);
}
int inla_read_prior_group1(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_prior_generic(mb, ini, sec, prior, "GROUP.PRIOR1", "GROUP.PARAMETERS1", "GROUP.FROM.THETA1", "GROUP.TO.THETA1", default_prior);
}
int inla_read_prior0(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 0);
}
int inla_read_prior1(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 1);
}
int inla_read_prior2(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 2);
}
int inla_read_prior3(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 3);
}
int inla_read_prior4(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 4);
}
int inla_read_prior5(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 5);
}
int inla_read_prior6(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior)
{
	return inla_read_priorN(mb, ini, sec, prior, default_prior, 6);
}
int inla_read_priorN(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *default_prior, int N)
{
	int val;
	char *a = NULL, *b = NULL, *c = NULL, *d = NULL;

	GMRFLib_sprintf(&a, "PRIOR%1d", N);
	GMRFLib_sprintf(&b, "PARAMETERS%1d", N);
	GMRFLib_sprintf(&c, "FROM.THETA%1d", N);
	GMRFLib_sprintf(&d, "TO.THETA%1d", N);
	val = inla_read_prior_generic(mb, ini, sec, prior, (const char *) a, (const char *) b, (const char *) c, (const char *) d, default_prior);
	Free(a);
	Free(b);
	Free(c);
	Free(d);

	return val;
}
int inla_read_prior_generic(inla_tp * mb, dictionary * ini, int sec, Prior_tp * prior, const char *prior_tag,
			    const char *param_tag, const char *from_theta, const char *to_theta, const char *default_prior)
{
	char *secname = NULL, *param = NULL;
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	prior->name = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, prior_tag), GMRFLib_strdup(default_prior)));

	if (!prior->name) {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, prior_tag, NULL);
	}

	prior->priorfunc = NULL;
	prior->expression = NULL;

	if (mb->verbose) {
		/*
		 * remove trailing -[a-zA-Z]*$ 
		 */
		char *p, *new_name;
		new_name = GMRFLib_strdup(prior->name);
		p = GMRFLib_rindex((const char *) new_name, '-');
		if (p) {
			*p = '\0';
		}
		p = GMRFLib_rindex((const char *) new_name, ':');
		if (p) {
			*p = '\0';
		}
		printf("\t\t%s->name=[%s]\n", prior_tag, new_name);
		Free(new_name);
	}

	prior->from_theta = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, from_theta), NULL));
	prior->to_theta = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, to_theta), NULL));
	if (mb->verbose) {
		printf("\t\t%s->from_theta=[%s]\n", prior_tag, prior->from_theta);
		printf("\t\t%s->to_theta = [%s]\n", prior_tag, prior->to_theta);
	}

	param = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, param_tag), NULL));
	if (!strcasecmp(prior->name, "LOGGAMMA")) {
		prior->id = P_LOGGAMMA;
		prior->priorfunc = priorfunc_loggamma;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = DEFAULT_GAMMA_PRIOR_A;
			prior->parameters[1] = DEFAULT_GAMMA_PRIOR_B;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "LOGGAMMA-ALPHA")) {
		prior->id = P_LOGGAMMA;
		prior->priorfunc = priorfunc_loggamma;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 25.0;
			prior->parameters[1] = 25.0;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN") || !strcasecmp(prior->name, "NORMAL")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;	       /* mean */
			prior->parameters[1] = DEFAULT_NORMAL_PRIOR_PRECISION;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN-1") || !strcasecmp(prior->name, "NORMAL-1")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 1.0;	       /* mean */
			prior->parameters[1] = 1.0;	       /* precision */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN-a") || !strcasecmp(prior->name, "NORMAL-a")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;	       /* mean */
			prior->parameters[1] = 6.25;	       /* precision */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN-std") || !strcasecmp(prior->name, "NORMAL-std")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;	       /* mean */
			prior->parameters[1] = 1.0;	       /* precision */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN-rho") || !strcasecmp(prior->name, "NORMAL-rho")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;	       /* mean */
			prior->parameters[1] = 0.2;	       /* precision */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "GAUSSIAN-group") || !strcasecmp(prior->name, "NORMAL-group")) {
		prior->id = P_GAUSSIAN;
		prior->priorfunc = priorfunc_gaussian;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;	       /* mean */
			prior->parameters[1] = 0.2;	       /* precision */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g, %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "MVNORM") || !strcasecmp(prior->name, "MVGAUSSIAN")) {
		int nparam, i, dim;
		double *tmp;

		prior->id = P_MVNORM;
		prior->priorfunc = priorfunc_mvnorm;
		inla_sread_doubles_q(&(prior->parameters), &nparam, param);
		if (mb->verbose) {
			for (i = 0; i < nparam; i++) {
				printf("\t\t%s->%s[%1d]=[%g]\n", prior_tag, param_tag, i, prior->parameters[i]);
			}
		}
		/*
		 * add the dimension of the mvgaussian as the first argument of the parameter 
		 */
		dim = -1;
		for (i = 0;; i++) {			       /* yes, an infinite loop */
			if (nparam == i + ISQR(i)) {
				dim = i;
				break;
			}
			if (nparam < i + ISQR(i)) {
				inla_error_general("nparam does not match with any dimension of the mvnorm");
				exit(EXIT_FAILURE);
			}
		}
		tmp = Calloc(nparam + 1, double);
		tmp[0] = dim;
		memcpy(&(tmp[1]), prior->parameters, nparam * sizeof(double));
		Free(prior->parameters);
		prior->parameters = tmp;
	} else if (!strcasecmp(prior->name, "PCSPDEGA")) {
		int nparam, i;

		prior->id = P_PC_SPDE_GA;
		prior->priorfunc = priorfunc_pc_spde_ga;
		inla_sread_doubles_q(&(prior->parameters), &nparam, param);
		assert(nparam == 4);
		if (mb->verbose) {
			for (i = 0; i < nparam; i++) {
				printf("\t\t%s->%s[%1d]=[%g]\n", prior_tag, param_tag, i, prior->parameters[i]);
			}
		}
	} else if (!strcasecmp(prior->name, "MINUSLOGSQRTRUNCNORMAL") || !strcasecmp(prior->name, "MINUSLOGSQRTRUNCGAUSSIAN") ||
		   // easier names...
		   !strcasecmp(prior->name, "LOGTNORMAL") || !strcasecmp(prior->name, "LOGTGAUSSIAN")) {
		prior->id = P_MINUSLOGSQRTRUNCGAUSSIAN;
		prior->priorfunc = priorfunc_minuslogsqrtruncnormal;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.0;
			prior->parameters[1] = DEFAULT_NORMAL_PRIOR_PRECISION;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "FLAT") || !strcasecmp(prior->name, "UNIFORM")) {
		/*
		 * do not care about the range for the FLAT/UNIFORM prior, as the parameters are already transformed to R. 
		 */
		prior->id = P_FLAT;
		prior->priorfunc = priorfunc_flat;
		prior->parameters = NULL;
	} else if (!strcasecmp(prior->name, "WISHART1D") ||
		   !strcasecmp(prior->name, "WISHART2D") ||
		   !strcasecmp(prior->name, "WISHART3D") || !strcasecmp(prior->name, "WISHART4D") || !strcasecmp(prior->name, "WISHART5D")) {

		if (!strcasecmp(prior->name, "WISHART1D")) {
			prior->id = P_WISHART1D;
			prior->priorfunc = priorfunc_wishart1d;
		} else if (!strcasecmp(prior->name, "WISHART2D")) {
			prior->id = P_WISHART2D;
			prior->priorfunc = priorfunc_wishart2d;
		} else if (!strcasecmp(prior->name, "WISHART3D")) {
			prior->id = P_WISHART3D;
			prior->priorfunc = priorfunc_wishart3d;
		} else if (!strcasecmp(prior->name, "WISHART4D")) {
			prior->id = P_WISHART4D;
			prior->priorfunc = priorfunc_wishart4d;
		} else if (!strcasecmp(prior->name, "WISHART5D")) {
			prior->id = P_WISHART5D;
			prior->priorfunc = priorfunc_wishart5d;
		} else {
			assert(0 == 1);
		}

		double *xx = NULL;
		int nxx;
		int idim = (!strcasecmp(prior->name, "WISHART1D") ? 1 :
			    (!strcasecmp(prior->name, "WISHART2D") ? 2 :
			     (!strcasecmp(prior->name, "WISHART3D") ? 3 :
			      (!strcasecmp(prior->name, "WISHART4D") ? 4 : (!strcasecmp(prior->name, "WISHART5D") ? 5 : -1)))));
		assert(idim > 0);

		inla_sread_doubles_q(&xx, &nxx, param);
		prior->parameters = xx;
		assert(nxx == inla_iid_wishart_nparam(idim) + 1);	/* this must be TRUE */

		if (mb->verbose) {
			int ii;
			for (ii = 0; ii < nxx; ii++) {
				printf("\t\t%s->%s prior_parameter[%1d] = %g\n", prior_tag, param_tag, ii, prior->parameters[ii]);
			}
		}
	} else if (!strcasecmp(prior->name, "LOGFLAT")) {
		prior->id = P_LOGFLAT;
		prior->priorfunc = priorfunc_logflat;
		prior->parameters = NULL;
		if (mb->verbose) {
			printf("\t\t%s->%s=[]\n", prior_tag, param_tag);
		}
	} else if (!strcasecmp(prior->name, "LOGIFLAT")) {
		prior->id = P_LOGIFLAT;
		prior->priorfunc = priorfunc_logiflat;
		prior->parameters = NULL;
		if (mb->verbose) {
			printf("\t\t%s->%s=[]\n", prior_tag, param_tag);
		}
	} else if (!strcasecmp(prior->name, "NONE")) {
		prior->id = P_NONE;
		prior->priorfunc = NULL;
		prior->parameters = NULL;
		if (mb->verbose) {
			printf("\t\t%s->%s=[]\n", prior_tag, param_tag);
		}
	} else if (!strcasecmp(prior->name, "BETACORRELATION")) {
		prior->id = P_BETACORRELATION;
		prior->priorfunc = priorfunc_betacorrelation;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 5.0;
			prior->parameters[1] = 5.0;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "LOGITBETA")) {
		prior->id = P_LOGITBETA;
		prior->priorfunc = priorfunc_betacorrelation;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 1.0;
			prior->parameters[1] = 1.0;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "PCPREC")) {
		prior->id = P_PC_PREC;
		prior->priorfunc = priorfunc_pc_prec;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.1;	       /* u */
			prior->parameters[1] = 0.001;	       /* alpha */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "PCDOF")) {
		prior->id = P_PC_DOF;
		prior->priorfunc = priorfunc_pc_dof;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double); /* Prob(dof < u) = alpha */
			prior->parameters[0] = 10.0;	       /* u */
			prior->parameters[1] = 0.5;	       /* alpha */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "LOGITBETA")) {
		prior->id = P_LOGITBETA;
		prior->priorfunc = priorfunc_logitbeta;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 5.0;
			prior->parameters[1] = 5.0;
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "PCRHO0")) {
		prior->id = P_PC_RHO0;
		prior->priorfunc = priorfunc_pc_rho0;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.5;	       /* u */
			prior->parameters[1] = 0.5;	       /* alpha */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "PCRHO1")) {
		prior->id = P_PC_RHO1;
		prior->priorfunc = priorfunc_pc_rho1;
		if (param && inla_is_NAs(2, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			if (inla_sread_doubles(prior->parameters, 2, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 0.5;	       /* u */
			prior->parameters[1] = 0.5;	       /* alpha */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g %g]\n", prior_tag, param_tag, prior->parameters[0], prior->parameters[1]);
		}
	} else if (!strcasecmp(prior->name, "PCAR")) {
		prior->id = P_PC_AR;
		prior->priorfunc = priorfunc_pc_ar;
		if (param && inla_is_NAs(1, param) != GMRFLib_SUCCESS) {
			prior->parameters = Calloc(2, double);
			double tmp;
			if (inla_sread_doubles(&tmp, 1, param) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, param_tag, param);
			}
			prior->parameters[0] = tmp;	       /* lambda */
			prior->parameters[1] = -1;	       /* ORDER: to be decided */
		} else {
			prior->parameters = Calloc(2, double);
			prior->parameters[0] = 1.0;	       /* lambda */
			prior->parameters[1] = -1;	       /* ORDER: to be decided */
		}
		if (mb->verbose) {
			printf("\t\t%s->%s=[%g]\n", prior_tag, param_tag, prior->parameters[0]);
		}
	} else if (!strncasecmp(prior->name, "EXPRESSION:", strlen("EXPRESSION:"))) {
		prior->id = P_EXPRESSION;
		prior->expression = GMRFLib_strdup(prior->name);
		prior->name[strlen("EXPRESSION")] = '\0';
		prior->parameters = NULL;

		if (mb->verbose) {
			printf("\t\t%s->%s=[%s]\n", prior_tag, prior->name, prior->expression);
		}

	} else if (!strncasecmp(prior->name, "TABLE:", strlen("TABLE:"))) {
		prior->id = P_TABLE;
		prior->expression = GMRFLib_strdup(prior->name);	/* yes, use the same storage */
		prior->name[strlen("TABLE")] = '\0';
		prior->parameters = NULL;

		if (mb->verbose) {
			printf("\t\t%s->%s=[%s]\n", prior_tag, prior->name, prior->expression);
		}

	} else if (!strcasecmp(prior->name, "JEFFREYSTDF")) {
		prior->id = P_JEFFREYS_T_DF;
		prior->priorfunc = priorfunc_jeffreys_df_student_t;
		prior->parameters = NULL;
		if (mb->verbose) {
			printf("\t\t%s->%s=[NULL]\n", prior_tag, param_tag);
		}
	} else {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, prior_tag, prior->name);
	}
	return INLA_OK;
}
inla_tp *inla_build(const char *dict_filename, int verbose, int make_dir)
{
	/*
	 * This function builds the model from the contents in INI 
	 */
	int found, sec, nsec, count, len, i, idx, j, k = -1;
	char *secname = NULL, *sectype = NULL, *sec_read = NULL, *msg = NULL;
	dictionary *ini = NULL;
	inla_tp *mb = NULL;

	if (verbose) {
		printf("%s...\n", __GMRFLib_FuncName);
	}
	mb = Calloc(1, inla_tp);
	mb->verbose = verbose;
	mb->sha1_hash = inla_inifile_sha1(dict_filename);
	inla_read_theta_sha1(&mb->sha1_hash_file, &mb->theta_file, &mb->ntheta_file);
	mb->reuse_mode = (mb->sha1_hash_file && strcmp((char *) mb->sha1_hash, (char *) mb->sha1_hash_file) == 0 ? 1 : 0);
	mb->reuse_mode = 0;				       /* disable this feature. creates more trouble than it solves. */
	if (mb->verbose && mb->reuse_mode) {
		printf("Reuse stored mode in [%s]\n", MODEFILENAME);
	}

	ini = iniparser_load(dict_filename);
	if (!ini) {
		GMRFLib_sprintf(&msg, "Fail to parse ini-file[%s]....", dict_filename);
		inla_error_general(msg);
	}
	nsec = iniparser_getnsec(ini);
	if (mb->verbose) {
		printf("\tnumber of sections=[%1d]\n", nsec);
	}
	/*
	 * first check that "type" is present in each section 
	 */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "type"), NULL)));
		if (!sectype) {
			inla_error_missing_required_field(__GMRFLib_FuncName, secname, "type");
		}
		Free(secname);
		Free(sectype);
	}
	sec_read = Calloc(nsec, char);


	/*
	 * default: gaussian data is on, then its turned off... 
	 */
	mb->gaussian_data = GMRFLib_TRUE;

	/*
	 * ...then parse the sections in this order: EXPERT, MODE, PROBLEM, PREDICTOR, DATA, FFIELD, LINEAR, INLA, UPDATE, LINCOMB, OUTPUT
	 * 
	 * it is easier to do it like this, instead of insisting the user to write the section in a spesific order.
	 * 
	 */
	for (sec = found = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "EXPERT")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[EXPERT]\n", sec, iniparser_getsecname(ini, sec));
			}
			if (found++) {
				GMRFLib_sprintf(&msg, "%s: two or more sections of type = [EXPERT]. Exit.\n", __GMRFLib_FuncName);
				inla_error_general(msg);
			}
			sec_read[sec] = 1;
			inla_parse_expert(mb, ini, sec);
		}
		Free(secname);
		Free(sectype);
	}

	for (sec = found = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "MODE")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[MODE]\n", sec, iniparser_getsecname(ini, sec));
			}
			if (found++) {
				GMRFLib_sprintf(&msg, "%s: two or more sections of type = [MODE]. Exit.\n", __GMRFLib_FuncName);
				inla_error_general(msg);
			}
			sec_read[sec] = 1;
			inla_parse_mode(mb, ini, sec);
		}
		Free(secname);
		Free(sectype);
	}

	for (sec = found = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "PROBLEM")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[PROBLEM]\n", sec, iniparser_getsecname(ini, sec));
			}
			if (found++) {
				GMRFLib_sprintf(&msg, "%s: two or more sections of type = [PROBLEM]. Exit.\n", __GMRFLib_FuncName);
				inla_error_general(msg);
			}
			sec_read[sec] = 1;
			inla_parse_problem(mb, ini, sec, make_dir);
		}
		Free(secname);
		Free(sectype);
	}
	if (!found) {
		GMRFLib_sprintf(&msg, "%s: no section of type = [PROBLEM]", __GMRFLib_FuncName);
		inla_error_general(msg);
	}

	/*
	 * type = predictor 
	 */
	for (sec = found = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "PREDICTOR")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[PREDICTOR]\n", sec, iniparser_getsecname(ini, sec));
			}
			if (found++) {
				GMRFLib_sprintf(&msg, "%s: two or more sections of type = [PREDICTOR]. Exit.\n", __GMRFLib_FuncName);
				inla_error_general(msg);
			}
			sec_read[sec] = 1;
			inla_parse_predictor(mb, ini, sec);
		}
		Free(secname);
		Free(sectype);
	}
	if (!found) {
		GMRFLib_sprintf(&msg, "%s: no section of type = [PREDICTOR]", __GMRFLib_FuncName);
		inla_error_general(msg);
	}

	/*
	 * type = DATA 
	 */
	mb->ds = 0;
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "DATA")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[DATA]\n", sec, iniparser_getsecname(ini, sec));
			}
			sec_read[sec] = 1;
			inla_parse_data(mb, ini, sec);
			mb->ds++;
		}
		Free(secname);
		Free(sectype);
	}
	if (!found) {
		GMRFLib_sprintf(&msg, "%s: no section of type [DATA] found", __GMRFLib_FuncName);
		inla_error_general(msg);
	}

	found = 0;
	/*
	 * type = ffield 
	 */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "FFIELD")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[FFIELD]\n", sec, iniparser_getsecname(ini, sec));
			}
			found++;
			sec_read[sec] = 1;
			inla_parse_ffield(mb, ini, sec);
		}
		Free(secname);
		Free(sectype);
	}

	inla_add_copyof(mb);

	/*
	 * type = linear 
	 */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "LINEAR")) {
			if (mb->verbose) {
				printf("\tsection=[%1d] name=[%s] type=[LINEAR]\n", sec, iniparser_getsecname(ini, sec));
			}
			found++;
			sec_read[sec] = 1;
			inla_parse_linear(mb, ini, sec);
		}
		Free(secname);
		Free(sectype);
	}

	/*
	 * type = UPDATE
	 */
	inla_setup_ai_par_default(mb);			       /* need this if there is no INLA section */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "UPDATE")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[UPDATE]\n", sec, iniparser_getsecname(ini, sec));
			}
			sec_read[sec] = 1;
			inla_parse_update(mb, ini, sec, make_dir);
		}
		Free(secname);
		Free(sectype);
	}

	/*
	 * build the index table and the hash; need this before reading the lincomb sections
	 */
	len = 1 + mb->nf + mb->nlinear;
	mb->idx_tag = Calloc(len, char *);
	mb->idx_start = Calloc(len, int);
	mb->idx_n = Calloc(len, int);

	j = idx = 0;
	mb->idx_tag[j] = GMRFLib_strdup(mb->predictor_tag);
	mb->idx_start[j] = idx;
	mb->idx_n[j] = mb->predictor_n + mb->predictor_m;

	idx += mb->idx_n[j++];
	for (i = 0; i < mb->nf; i++) {
		mb->idx_tag[j] = GMRFLib_strdup(mb->f_tag[i]);
		mb->idx_start[j] = idx;
		mb->idx_n[j] = mb->f_Ntotal[i];
		idx += mb->idx_n[j++];
	}
	for (i = 0; i < mb->nlinear; i++) {
		mb->idx_tag[j] = GMRFLib_strdup(mb->linear_tag[i]);
		mb->idx_start[j] = idx;
		mb->idx_n[j] = 1;
		idx += mb->idx_n[j++];
	}
	mb->idx_tot = j;
	mb->idx_ntot = idx;

	map_stri_init_hint(&(mb->idx_hash), mb->idx_tot);
	for (i = 0; i < mb->idx_tot; i++) {
		map_stri_set(&(mb->idx_hash), GMRFLib_strdup(mb->idx_tag[i]), i);
	}

	if (mb->verbose) {
		printf("\tIndex table: number of entries[%1d], total length[%1d]\n", mb->idx_tot, mb->idx_ntot);
		printf("\t\t%-30s %10s %10s\n", "tag", "start-index", "length");
		for (i = 0; i < mb->idx_tot; i++) {
			printf("\t\t%-30s %10d %10d\n", mb->idx_tag[i], mb->idx_start[i], mb->idx_n[i]);
		}
	}

	/*
	 * type = INLA 
	 */
	inla_setup_ai_par_default(mb);			       /* need this if there is no INLA section */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "INLA")) {
			if (mb->verbose) {
				printf("\tparse section=[%1d] name=[%s] type=[INLA]\n", sec, iniparser_getsecname(ini, sec));
			}
			sec_read[sec] = 1;
			inla_parse_INLA(mb, ini, sec, make_dir);
		}
		Free(secname);
		Free(sectype);
	}


	/*
	 * type = lincomb
	 */
	int numsec = 0, *secmap = NULL, isec = -1;

	/*
	 * first we count them
	 */
	for (sec = 0; sec < nsec; sec++) {
		secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
		sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
		if (!strcmp(sectype, "LINCOMB")) {
			numsec++;
		}
	}
	if (numsec > 0) {
		/*
		 * then we find out which order to read them using 'LINCOMB.ORDER'
		 */
		secmap = Calloc(numsec, int);
		for (sec = 0; sec < nsec; sec++) {
			secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
			sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
			if (!strcmp(sectype, "LINCOMB")) {
				int ordering;
				ordering = (int) iniparser_getdouble(ini, inla_string_join((const char *) secname, "LINCOMB.ORDER"), -1);
				GMRFLib_ASSERT_RETVAL(ordering > 0, GMRFLib_ESNH, (inla_tp *) NULL);
				secmap[ordering - 1] = sec;    /* ordering in the Model.ini is from 1...n */
			}
		}

		/*
		 * then we read them
		 */
		for (isec = 0; isec < numsec; isec++) {
			sec = secmap[isec];
			secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
			sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
			if (!strcmp(sectype, "LINCOMB")) {
				/*
				 * we need to implement this here, as the number of linear combinations can get really huge and we need to surpress the verbose mode just
				 * for these sections. 
				 */
				int verbose_save = mb->verbose;

				// This option can surpress mb->verbose locally, but not the other way around.
				mb->verbose = iniparser_getint(ini, inla_string_join(secname, "VERBOSE"), mb->verbose) && mb->verbose;

				if (mb->verbose) {
					printf("\tsection=[%1d] name=[%s] type=[LINCOMB]\n", sec, iniparser_getsecname(ini, sec));
				}
				found++;
				sec_read[sec] = 1;
				inla_parse_lincomb(mb, ini, sec);

				mb->verbose = verbose_save;    /* set it back */
			} else {
				/*
				 * should not happen
				 */
				assert(0 == 1);
			}
			Free(secname);
			Free(sectype);
		}
	}
	if (mb->verbose) {
		if (numsec) {
			printf("\tRead [%1d] sections with mode=[LINCOMB]\n", numsec);
		}
	}
	Free(secmap);

	/*
	 * check that all sections are read 
	 */
	for (sec = 0; sec < nsec; sec++) {
		if (!sec_read[sec]) {
			secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
			sectype = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join((const char *) secname, "TYPE"), NULL)));
			GMRFLib_sprintf(&msg, "%s: section=[%s] is not used; please check its type=[%s]", __GMRFLib_FuncName, secname, sectype);
			inla_error_general(msg);
		}
	}
	if (mb->verbose) {
		printf("%s: check for unused entries in[%s]\n", __GMRFLib_FuncName, dict_filename);
	}
	if ((count = dictionary_dump_unused(ini, stderr))) {
		fprintf(stderr, "\n\ninla_build: [%s] contain[%1d] unused entries. PLEASE CHECK\n", dict_filename, count);
		exit(EXIT_FAILURE);
	}

	if (mb->reuse_mode) {
		/*
		 * if the test fail, its a good idea to provide some debug information which might be helpful to help what is wrong in the spesification. 
		 */
		if (mb->theta_counter_file != mb->ntheta_file) {
			P(mb->theta_counter_file);
			P(mb->ntheta_file);
		}
		assert(mb->theta_counter_file == mb->ntheta_file);
	}

	/*
	 * make the final likelihood from all the data-sections 
	 */
	mb->loglikelihood = Calloc(mb->predictor_ndata, GMRFLib_logl_tp *);
	mb->loglikelihood_arg = Calloc(mb->predictor_ndata, void *);
	mb->d = Calloc(mb->predictor_ndata, double);
	mb->family_idx = Calloc(mb->predictor_ndata, double);
	mb->len_family_idx = mb->predictor_ndata;

	for (i = 0; i < mb->predictor_ndata; i++) {
		for (j = found = 0; j < mb->nds; j++) {
			if (mb->data_sections[j].data_observations.d[i]) {
				k = j;
				found++;
			}
		}
		if (found > 1) {
			GMRFLib_sprintf(&msg, "Observation %d occurs in more than one data-section\n", i);
			inla_error_general(msg);
			exit(EXIT_FAILURE);
		}

		if (found) {
			mb->family_idx[i] = k;
			mb->loglikelihood[i] = mb->data_sections[k].loglikelihood;
			mb->loglikelihood_arg[i] = (void *) &(mb->data_sections[k]);
			mb->d[i] = mb->data_sections[k].data_observations.d[i];
		} else {
			mb->family_idx[i] = NAN;
			mb->loglikelihood[i] = NULL;
			mb->loglikelihood_arg[i] = NULL;
			mb->d[i] = 0.0;
		}
	}
	mb->data_ntheta_all = 0;
	for (j = 0; j < mb->nds; j++) {
		mb->data_ntheta_all += mb->data_sections[j].data_ntheta + mb->data_sections[j].mix_ntheta + mb->data_sections[j].link_ntheta;
		mb->data_sections[j].offset = mb->offset;      /* just a copy */
		mb->data_sections[j].mb = mb;		       /* just a copy */
	}

	/*
	 * make the final predictor_... from all the data-sections 
	 */
	mb->predictor_invlinkfunc = Calloc(mb->predictor_n + mb->predictor_m, link_func_tp *);
	mb->predictor_invlinkfunc_arg = Calloc(mb->predictor_n + mb->predictor_m, void *);
	mb->predictor_invlinkfunc_covariates = Calloc(mb->predictor_n + mb->predictor_m, GMRFLib_matrix_tp *);
	for (i = 0; i < mb->predictor_ndata; i++) {
		for (j = found = 0; j < mb->nds; j++) {
			if (mb->data_sections[j].data_observations.d[i]) {
				k = j;
				found++;
			}
		}
		if (found > 1) {
			GMRFLib_sprintf(&msg, "Observation %d occurs in more than one data-section\n", i);
			inla_error_general(msg);
			exit(EXIT_FAILURE);
		}
		mb->predictor_invlinkfunc[i] = (found == 1 ? mb->data_sections[k].predictor_invlinkfunc :
						(mb->link_fitted_values && !gsl_isnan(mb->link_fitted_values[i])) ?
						mb->data_sections[(int) (mb->link_fitted_values[i])].predictor_invlinkfunc : NULL);
		mb->predictor_invlinkfunc_arg[i] = (found == 1 ? mb->data_sections[k].predictor_invlinkfunc_arg :
						    (mb->link_fitted_values && !gsl_isnan(mb->link_fitted_values[i])) ?
						    mb->data_sections[(int) (mb->link_fitted_values[i])].predictor_invlinkfunc_arg : NULL);
		mb->predictor_invlinkfunc_covariates[i] = (found == 1 ? mb->data_sections[k].link_covariates :
							   (mb->link_fitted_values && !gsl_isnan(mb->link_fitted_values[i])) ?
							   mb->data_sections[(int) (mb->link_fitted_values[i])].link_covariates : NULL);
	}

	iniparser_freedict(ini);
	return mb;
}
int inla_tolower(char *string)
{
	if (string) {
		int i;
		for (i = 0; i < (int) strlen(string); i++) {
			string[i] = (char) tolower((int) string[i]);
		}
	}
	return GMRFLib_SUCCESS;
}
int inla_parse_lincomb(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = LINCOMB. Here we assume the binary files are written by Rinla, so they are index-1 based!!!!!
	 */
	int *ip = NULL, num_sections, sec_no, n, npairs, debug = 0, offset, i;
	size_t fileoffset = 0;
	char *filename = NULL, *secname = NULL, *ptr = NULL, *msg = NULL;
	GMRFLib_io_tp *io = NULL;
	GMRFLib_lc_tp *lc = NULL;

	mb->lc_tag = Realloc(mb->lc_tag, mb->nlc + 1, char *);
	mb->lc_output = Realloc(mb->lc_output, mb->nlc + 1, Output_tp *);
	mb->lc_dir = Realloc(mb->lc_dir, mb->nlc + 1, char *);
	mb->lc_prec = Realloc(mb->lc_prec, mb->nlc + 1, double);
	mb->lc_order = Realloc(mb->lc_order, mb->nlc + 1, double);
	mb->lc_tag[mb->nlc] = secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	mb->lc_dir[mb->nlc] = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "DIR"), inla_fnmfix(GMRFLib_strdup(mb->lc_tag[mb->nlc]))));

	if (mb->verbose) {
		printf("\tinla_parse_lincomb...\n\t\tsecname = [%s]\n", mb->lc_tag[mb->nlc]);
	}

	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FILENAME"), NULL));
	if (!filename) {
		inla_error_missing_required_field(__GMRFLib_FuncName, secname, "filename");
	}
	fileoffset = (size_t) iniparser_getdouble(ini, inla_string_join(secname, "FILE.OFFSET"), 0.0);

	if (mb->verbose) {
		printf("\t\tfilename [%s]\n", filename);
		printf("\t\tfile.offset [%lu]\n", (long unsigned) fileoffset);
	}

	mb->lc_order[mb->nlc] = iniparser_getdouble(ini, inla_string_join(secname, "LINCOMB.ORDER"), -1.0);
	assert(mb->lc_order[mb->nlc] >= 0);

	mb->lc_prec[mb->nlc] = iniparser_getdouble(ini, inla_string_join(secname, "PRECISION"), 1.0e9);
	if (mb->verbose) {
		printf("\t\tprecision [%g]\n", mb->lc_prec[mb->nlc]);
	}
	// FORMAT:: se section.R in Rinla...

	GMRFLib_io_open(&io, filename, "rb");
	if (fileoffset > 0)
		GMRFLib_io_seek(io, fileoffset, SEEK_SET);

	if (mb->verbose) {
		printf("\t\tOpen file [%s] at location [%lu]\n", filename, (long unsigned) fileoffset);
	}

	GMRFLib_io_read(io, &num_sections, sizeof(int));
	if (mb->verbose) {
		printf("\t\tNumber of sections [%d]\n", num_sections);
	}

	lc = Calloc(1, GMRFLib_lc_tp);
	lc->n = 0;
	lc->idx = NULL;
	lc->weight = NULL;
	lc->tinfo = Calloc(ISQR(GMRFLib_MAX_THREADS), GMRFLib_lc_tinfo_tp);
	for (i = 0; i < ISQR(GMRFLib_MAX_THREADS); i++) {
		lc->tinfo[i].first_nonzero = -1;
		lc->tinfo[i].last_nonzero = -1;
		lc->tinfo[i].first_nonzero_mapped = -1;
		lc->tinfo[i].last_nonzero_mapped = -1;
	}

	int all_weights_are_zero = 1;
	for (sec_no = 0; sec_no < num_sections; sec_no++) {

		int len;

		GMRFLib_io_read(io, &len, sizeof(int));
		ptr = Calloc(len + 1, char);
		GMRFLib_io_read(io, ptr, len + 1);	       /* includes trailing \0 */
		if (mb->verbose) {
			printf("\t\t\tSection [%1d] is named [%s]\n", sec_no, ptr);
		}
		ip = map_stri_ptr(&(mb->idx_hash), ptr);
		if (!ip) {
			GMRFLib_sprintf(&msg, "Section no [%1d] named [%s] in file [%1d] offset[%16.0g] is unknown.", sec_no, ptr, filename, (double) fileoffset);
			GMRFLib_io_close(io);
			inla_error_general(msg);
		}

		offset = mb->idx_start[*ip];
		n = mb->idx_n[*ip];

		if (mb->verbose)
			printf("\t\t\tSection has offset=[%1d] and n=[%1d]\n", offset, n);

		GMRFLib_io_read(io, &npairs, sizeof(int));
		assert(npairs >= 0);

		if (mb->verbose) {
			printf("\t\t\tnpairs=[%1d]\n", npairs);
		}

		int *idx = Calloc(npairs, int);
		double *w = Calloc(npairs, double);

		GMRFLib_io_read(io, idx, npairs * sizeof(int));
		lc->idx = Realloc(lc->idx, lc->n + npairs, int);
		for (i = 0; i < npairs; i++) {
			lc->idx[lc->n + i] = (idx[i] - 1) + offset;	/* `-1': convert to C-indexing */

			/*
			 * check that the index is legal
			 */
			if (!LEGAL(idx[i] - 1, n)) {
				fprintf(stderr, "\n\n");
				fprintf(stderr, "*** ERROR ***\tLincomb error for section[%s]\n", secname);
				fprintf(stderr, "*** ERROR ***\t[%s] has length %1d, but idx=%1d (R-style index) is given\n", ptr, n, idx[i]);
				GMRFLib_ASSERT(0 == 1, GMRFLib_EPARAMETER);
			}
		}


		GMRFLib_io_read(io, w, npairs * sizeof(double));
		lc->weight = Realloc(lc->weight, lc->n + npairs, float);	/* YES! */
		for (i = 0; i < npairs; i++) {
			lc->weight[lc->n + i] = (float) w[i];
			all_weights_are_zero &= (w[i] == 0.0);
		}

		Free(idx);
		Free(w);
		Free(ptr);

		if (debug) {
			for (i = 0; i < npairs; i++) {
				printf("\t\t\t\tC.idx+offset [%1d] weight [%g]\n", lc->idx[lc->n + i], lc->weight[lc->n + i]);
			}
		}

		lc->n += npairs;
	}
	GMRFLib_io_close(io);

	if (all_weights_are_zero) {
		fprintf(stderr, "\n\n");
		fprintf(stderr, "*** ERROR ***\tLincomb error for section[%s]\n", secname);
		fprintf(stderr, "*** ERROR ***\tAll weights are zero.\n");
		GMRFLib_ASSERT(0 == 1, GMRFLib_EPARAMETER);
	}

	/*
	 * sort them with increasing idx's (and carry the weights along) to speed things up later on. 
	 */
	GMRFLib_qsorts((void *) lc->idx, (size_t) lc->n, sizeof(int), (void *) lc->weight, sizeof(float), NULL, 0, GMRFLib_icmp);
	if (mb->verbose) {
		printf("\t\tNumber of non-zero weights [%1d]\n", lc->n);
		printf("\t\tLincomb = \tidx \tweight\n");
		for (i = 0; i < IMIN(lc->n, PREVIEW); i++) {
			printf("\t\t\t%6d \t\t%.10f\n", lc->idx[i], lc->weight[i]);
		}
	}
	mb->lc_lc = Realloc(mb->lc_lc, (mb->nlc + 1), GMRFLib_lc_tp *);
	mb->lc_lc[mb->nlc] = lc;
	inla_parse_output(mb, ini, sec, &(mb->lc_output[mb->nlc]));
	mb->nlc++;

	return INLA_OK;
}
int inla_parse_mode(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = MODE
	 */
	int nt = 0, i;
	char *tmp, *secname;
	double *t = NULL;
	FILE *fp;
	size_t nread;

	if (mb->verbose) {
		printf("\tinla_parse_mode...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	tmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "THETA"), NULL));

	/*
	 * first try if 'tmp' is a filename, is so, read (using binary format) from that. format: NTHETA theta[0] theta[1] .... theta[ NTHETA-1 ] 
	 */

	if (tmp) {
		fp = fopen(tmp, "rb");
		if (fp) {
			nread = fread(&(mb->ntheta_file), sizeof(int), 1, fp);
			assert(nread == 1);
			mb->theta_file = Calloc(mb->ntheta_file, double);
			nread = fread(mb->theta_file, sizeof(double), mb->ntheta_file, fp);
			assert(nread == (size_t) mb->ntheta_file);
			fclose(fp);

			mb->reuse_mode = 1;
		} else {
			inla_sread_doubles_q(&t, &nt, tmp);
			if (nt) {
				mb->ntheta_file = nt;
				mb->theta_file = t;
				mb->reuse_mode = 1;
			} else {
				mb->ntheta_file = 0;
				mb->theta_file = NULL;
				mb->reuse_mode = 0;
				Free(t);
			}
		}
	} else {
		mb->ntheta_file = 0;
		mb->theta_file = NULL;
		mb->reuse_mode = 0;
	}

	if (mb->verbose) {
		if (mb->ntheta_file) {
			printf("\tUse mode in section[%s]\n", secname);
			printf("\t\ttheta = ");
			for (i = 0; i < mb->ntheta_file; i++) {
				printf(" %.4g", mb->theta_file[i]);
			}
			printf("\n");
		} else {
			printf("\tDid not find any mode in section[%s]\n", secname);
		}
	}

	tmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "X"), NULL));
	if (tmp) {
		/*
		 * this is new code that use binary i/o 
		 */
		// format: NX x[0] x[1] .... x[ NX-1 ]
		fp = fopen(tmp, "rb");
		nread = fread(&(mb->nx_file), sizeof(int), 1, fp);
		assert(nread == 1);
		mb->x_file = Calloc(mb->nx_file, double);
		nread = fread(mb->x_file, sizeof(double), mb->nx_file, fp);
		assert(nread == (size_t) mb->nx_file);
		fclose(fp);

		if (mb->verbose) {
			printf("\t\tx = ");
			for (i = 0; i < IMIN(mb->nx_file, PREVIEW); i++) {
				printf(" %.4g", mb->x_file[i]);
			}
			printf(" ...\n");
		}
	}

	mb->reuse_mode_but_restart = iniparser_getboolean(ini, inla_string_join(secname, "RESTART"), 0);
	if (mb->verbose) {
		printf("\t\tRestart = %1d\n", mb->reuse_mode_but_restart);
	}

	mb->fixed_mode = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
	if (mb->verbose) {
		printf("\t\tFixed mode= %1d\n", mb->fixed_mode);
	}

	return INLA_OK;
}
int inla_parse_problem(inla_tp * mb, dictionary * ini, int sec, int make_dir)
{
	/*
	 * parse section = PROBLEM
	 */
	int i, ok;
	char *secname = NULL, *tmp = NULL, *tmpp = NULL, *smtp = NULL, *openmp_strategy = NULL;

	mb->predictor_tag = secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\tinla_parse_problem...\n");
	}
	mb->name = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "NAME"), NULL));
	if (!mb->name) {
		mb->name = GMRFLib_strdup(secname);
	}
	if (mb->verbose) {
		printf("\t\tname=[%s]\n", mb->name);
	}
	openmp_strategy = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "OPENMP.STRATEGY"), GMRFLib_strdup("DEFAULT")));
	if (mb->verbose) {
		printf("\t\topenmp.strategy=[%s]\n", openmp_strategy);
	}

	if (!strcasecmp(openmp_strategy, "DEFAULT")) {
		/*
		 * this option means that it will be determined later on. 
		 */
		mb->strategy = GMRFLib_OPENMP_STRATEGY_DEFAULT;
	} else if (!strcasecmp(openmp_strategy, "SMALL")) {
		mb->strategy = GMRFLib_OPENMP_STRATEGY_SMALL;
	} else if (!strcasecmp(openmp_strategy, "MEDIUM")) {
		mb->strategy = GMRFLib_OPENMP_STRATEGY_MEDIUM;
	} else if (!strcasecmp(openmp_strategy, "LARGE")) {
		mb->strategy = GMRFLib_OPENMP_STRATEGY_LARGE;
	} else if (!strcasecmp(openmp_strategy, "HUGE")) {
		mb->strategy = GMRFLib_OPENMP_STRATEGY_HUGE;
	} else {
		GMRFLib_sprintf(&tmp, "Unknown openmp.strategy [%s]", openmp_strategy);
		inla_error_general(tmp);
		exit(EXIT_FAILURE);
	}

	smtp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SMTP"), NULL));
	if (smtp) {
		if (!strcasecmp(smtp, "GMRFLib_SMTP_BAND") || !strcasecmp(smtp, "BAND")) {
			GMRFLib_smtp = GMRFLib_SMTP_BAND;
		} else if (!strcasecmp(smtp, "GMRFLib_SMTP_TAUCS") || !strcasecmp(smtp, "TAUCS")) {
			GMRFLib_smtp = GMRFLib_SMTP_TAUCS;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "smtp", smtp);
		}
		if (mb->verbose) {
			printf("\t\tsmtp=[%s]\n", smtp);
		}
	}
	mb->dir = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "DIR"), GMRFLib_strdup("results-%1d")));
	ok = 0;
	int accept_argument = 0;

	if (make_dir) {
		GMRFLib_sprintf(&tmp, mb->dir, 0);
		GMRFLib_sprintf(&tmpp, mb->dir, 99);
		accept_argument = (strcmp(tmp, tmpp) == 0 ? 0 : 1);
		Free(tmp);
		Free(tmpp);
		for (i = 0; i < 9999; i++) {
			GMRFLib_sprintf(&tmp, mb->dir, i);
			inla_fnmfix(tmp);
			if (inla_mkdir(tmp) != 0) {
				if (mb->verbose) {
					printf("\t\tfail to create directory [%s]: %s\n", tmp, strerror(errno));
				}
				if (!accept_argument) {
					fprintf(stderr, "\n\t\tFail to create directory [%s]: %s\n", tmp, strerror(errno));
					fprintf(stderr, "\t\tmb->dir=[%s] does not accept integer arguments. Cannot proceed.\n\n", mb->dir);
					exit(EXIT_FAILURE);
				}
			} else {
				if (mb->verbose) {
					printf("\tstore results in directory=[%s]\n", tmp);
				}
				mb->dir = tmp;
				ok = 1;
				break;
			}
			Free(tmp);
		}
		if (!ok) {
			inla_error_general("Fail to create directory. I give up.");
		}
	}
	inla_parse_output(mb, ini, sec, &(mb->output));
	return INLA_OK;
}
int inla_parse_predictor(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = PREDICTOR 
	 */
	char *secname = NULL, *msg = NULL, *filename;
	int i, noffsets, nlinks_fitted_values;
	double tmp;

	if (mb->verbose) {
		printf("\tinla_parse_predictor ...\n");
	}
	mb->predictor_tag = secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (!mb->predictor_tag) {
		mb->predictor_tag = GMRFLib_strdup("predictor");
	}
	if (mb->verbose) {
		printf("\t\tsection=[%s]\n", secname);
	}
	mb->predictor_dir = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "DIR"), inla_fnmfix(GMRFLib_strdup(mb->predictor_tag))));
	if (mb->verbose) {
		printf("\t\tdir=[%s]\n", mb->predictor_dir);
	}

	inla_read_prior(mb, ini, sec, &(mb->predictor_prior), "LOGGAMMA");

	tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
	mb->predictor_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
	if (!mb->predictor_fixed && mb->reuse_mode) {
		tmp = mb->theta_file[mb->theta_counter_file++];
	}
	HYPER_NEW(mb->predictor_log_prec, tmp);
	if (mb->verbose) {
		printf("\t\tinitialise log_precision[%g]\n", mb->predictor_log_prec[0][0]);
		printf("\t\tfixed=[%1d]\n", mb->predictor_fixed);
	}

	if (!mb->predictor_fixed) {
		mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
		mb->theta[mb->ntheta] = mb->predictor_log_prec;
		mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
		mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
		mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

		mb->theta_tag[mb->ntheta] = GMRFLib_strdup("Log precision for the linear predictor");
		mb->theta_tag_userscale[mb->ntheta] = GMRFLib_strdup("Precision for the linear predictor");
		mb->theta_dir[mb->ntheta] = GMRFLib_strdup(mb->predictor_dir);

		mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
		mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
		mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->predictor_prior.from_theta);
		mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->predictor_prior.to_theta);

		mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
		mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
		mb->theta_map_arg[mb->ntheta] = NULL;
		mb->theta_map[mb->ntheta] = map_precision;
		mb->ntheta++;
	}
	mb->predictor_user_scale = iniparser_getboolean(ini, inla_string_join(secname, "USER.SCALE"), 1);
	if (mb->verbose) {
		printf("\t\tuser.scale=[%1d]\n", mb->predictor_user_scale);
	}

	mb->predictor_n = iniparser_getint(ini, inla_string_join(secname, "N"), -1);
	assert(mb->predictor_n > 0);
	if (mb->verbose) {
		printf("\t\tn=[%1d]\n", mb->predictor_n);
	}
	mb->predictor_m = iniparser_getint(ini, inla_string_join(secname, "M"), 0);
	assert(mb->predictor_m >= 0);
	if (mb->verbose) {
		printf("\t\tm=[%1d]\n", mb->predictor_m);
	}

	if (mb->predictor_m == 0) {
		mb->predictor_ndata = mb->predictor_n;
	} else {
		mb->predictor_ndata = mb->predictor_m;
	}
	if (mb->verbose) {
		printf("\t\tndata=[%1d]\n", mb->predictor_ndata);
	}

	mb->predictor_compute = iniparser_getboolean(ini, inla_string_join(secname, "COMPUTE"), 1);	// mb->output->cpo || mb->output->dic
	if (G.mode == INLA_MODE_HYPER) {
		if (mb->predictor_compute) {
			fprintf(stderr, "*** Warning: HYPER_MODE require predictor_compute = 0\n");
		}
		mb->predictor_compute = 0;
	}
	if (mb->verbose) {
		printf("\t\tcompute=[%1d]\n", mb->predictor_compute);
	}
	if ((mb->output->cpo || mb->output->dic) && !mb->predictor_compute) {
		GMRFLib_sprintf(&msg, "Illegal combination: output->cpo or dic = 1, require predictor->compute = 1, but predictor->compute = 0");
		inla_error_general(msg);
	}

	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "OFFSET"), NULL));
	if (filename) {
		if (mb->verbose) {
			printf("\t\tread offsets from file=[%s]\n", filename);
		}
		inla_read_data_general(&(mb->offset), NULL, &noffsets, filename, mb->predictor_n + mb->predictor_m, 0, 1, mb->verbose, 0.0);
	} else {
		mb->offset = Calloc(mb->predictor_n + mb->predictor_m, double);
	}

	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "LINK.FITTED.VALUES"), NULL));
	if (filename) {
		if (mb->verbose) {
			printf("\t\tread link.fitted.values from file=[%s]\n", filename);
		}
		inla_read_data_general(&(mb->link_fitted_values), NULL, &nlinks_fitted_values, filename, mb->predictor_n + mb->predictor_m, 0, 1, mb->verbose, 0.0);
	} else {
		mb->link_fitted_values = NULL;
	}

	if (0) {
		if (mb->link_fitted_values) {
			for (i = 0; i < mb->predictor_n + mb->predictor_m; i++)
				if (gsl_isnan(mb->link_fitted_values[i]))
					fprintf(stderr, "link[%d] = NAN\n", i);
				else
					fprintf(stderr, "link[%d] = %g (%g)\n", i, mb->link_fitted_values[i], GSL_NAN);
		}
	}

	mb->predictor_cross_sumzero = NULL;
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CROSS_CONSTRAINT"), NULL));
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CROSS.CONSTRAINT"), filename));
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CROSSCONSTRAINT"), filename));

	double *dcross = NULL;
	int *icross = NULL, len_cross = 0, nu = 0;

	if (filename) {
		inla_read_data_all(&dcross, &len_cross, filename);
		if (len_cross > 0) {
			if (len_cross != mb->predictor_n + mb->predictor_m) {
				GMRFLib_sprintf(&msg, "Length of cross-sum-to-zero is not equal to the TOTAL length of linear predictor: %1d != %1d\n",
						len_cross, mb->predictor_n + mb->predictor_m);
			}
			icross = Calloc(len_cross, int);
			for (i = 0; i < len_cross; i++)
				icross[i] = (int) dcross[i];
			Free(dcross);
			mb->predictor_cross_sumzero = icross;
		}
	}
	if (mb->verbose && mb->predictor_cross_sumzero) {
		GMRFLib_iuniques(&nu, NULL, mb->predictor_cross_sumzero, mb->predictor_n);
		printf("\t\tread cross-sum-to-zero from file[%s]: %1d constraints\n", filename, nu);
		for (i = 0; i < IMIN(PREVIEW, mb->predictor_n + mb->predictor_m); i++) {
			printf("\t\t\t%1d %1d\n", i, mb->predictor_cross_sumzero[i]);
		}
	}

	/*
	 * these are for the extended observational model. only valid if predictor_m > 0.
	 */
	mb->predictor_Aext_fnm = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "AEXT"), NULL));
	mb->predictor_Aext_precision = iniparser_getdouble(ini, inla_string_join(secname, "AEXTPRECISION"), 1.0e8);

	if (mb->verbose) {
		printf("\t\tAext=[%s]\n", mb->predictor_Aext_fnm);
		printf("\t\tAextPrecision=[%.4g]\n", mb->predictor_Aext_precision);
	}
	if (mb->predictor_m > 0) {
		assert(mb->predictor_Aext_fnm != NULL);
	}
	if (mb->predictor_m == 0) {
		assert(mb->predictor_Aext_fnm == NULL);
	}

	inla_parse_output(mb, ini, sec, &(mb->predictor_output));

	return INLA_OK;
}
int inla_trim_family(char *family)
{
	size_t i, j = 0;

	assert(family);
	for (i = 0, j = 0; i < strlen(family); i++) {
		if (family[i] != '.' && family[i] != '_' && family[i] != ' ' && family[i] != '\t') {
			family[j] = family[i];
			j++;
		}
	}
	family[j] = '\0';
	return GMRFLib_SUCCESS;
}
char *inla_make_tag(const char *string, int ds)
{
	char *res;

	if (ds > 0) {
		GMRFLib_sprintf(&res, "%s[%1d]", string, ds + 1);	/* yes, number these from 1...nds */
	} else {
		res = GMRFLib_strdup(string);
	}

	return res;
}
int inla_parse_data(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = DATA 
	 */

	char *secname = NULL, *msg = NULL;
	int i;
	double tmp;
	Data_section_tp *ds;

	mb->nds++;
	mb->data_sections = Realloc(mb->data_sections, mb->nds, Data_section_tp);
	ds = &(mb->data_sections[mb->nds - 1]);		       /* shorthand */
	memset(ds, 0, sizeof(Data_section_tp));

	if (mb->verbose) {
		printf("\tinla_parse_data [section %1d]...\n", mb->nds);
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\ttag=[%s]\n", secname);
	}
	ds->data_likelihood = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join(secname, "LIKELIHOOD"), NULL)));
	inla_trim_family(ds->data_likelihood);

	if (mb->verbose) {
		printf("\t\tfamily=[%s]\n", ds->data_likelihood);
	}

	if (!(ds->data_likelihood)) {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, "LIKELIHOOD", ds->data_likelihood);
	}
	if (!strcasecmp(ds->data_likelihood, "GAUSSIAN") || !strcasecmp(ds->data_likelihood, "NORMAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_gaussian;
		ds->data_id = L_GAUSSIAN;
	} else if (!strcasecmp(ds->data_likelihood, "SIMPLEX")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_simplex;
		ds->data_id = L_SIMPLEX;
	} else if (!strcasecmp(ds->data_likelihood, "IIDGAMMA")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_iid_gamma;
		ds->data_id = L_IID_GAMMA;
	} else if (!strcasecmp(ds->data_likelihood, "IIDLOGITBETA")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_iid_logitbeta;
		ds->data_id = L_IID_LOGITBETA;
	} else if (!strcasecmp(ds->data_likelihood, "LOGGAMMAFRAILTY")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_loggamma_frailty;
		ds->data_id = L_LOGGAMMA_FRAILTY;
	} else if (!strcasecmp(ds->data_likelihood, "LOGISTIC")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_logistic;
		ds->data_id = L_LOGISTIC;
	} else if (!strcasecmp(ds->data_likelihood, "SKEWNORMAL") || !strcasecmp(ds->data_likelihood, "SN")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_skew_normal;
		ds->data_id = L_SKEWNORMAL;
	} else if (!strcasecmp(ds->data_likelihood, "SKEWNORMAL2") || !strcasecmp(ds->data_likelihood, "SN2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_skew_normal2;
		ds->data_id = L_SKEWNORMAL2;
	} else if (!strcasecmp(ds->data_likelihood, "GEV")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_gev;
		ds->data_id = L_GEV;
	} else if (!strcasecmp(ds->data_likelihood, "LAPLACE")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_laplace;
		ds->data_id = L_LAPLACE;
	} else if (!strcasecmp(ds->data_likelihood, "T")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_t;
		ds->data_id = L_T;
	} else if (!strcasecmp(ds->data_likelihood, "TSTRATA")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_tstrata;
		ds->data_id = L_TSTRATA;
	} else if (!strcasecmp(ds->data_likelihood, "POISSON")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_poisson;
		ds->data_id = L_POISSON;
	} else if (!strcasecmp(ds->data_likelihood, "GPOISSON")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_gpoisson;
		ds->data_id = L_GPOISSON;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDPOISSON0")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_poisson0;
		ds->data_id = L_ZEROINFLATEDPOISSON0;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDPOISSON1")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_poisson1;
		ds->data_id = L_ZEROINFLATEDPOISSON1;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDPOISSON2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_poisson2;
		ds->data_id = L_ZEROINFLATEDPOISSON2;
	} else if (!strcasecmp(ds->data_likelihood, "BINOMIAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_binomial;
		ds->data_id = L_BINOMIAL;
	} else if (!strcasecmp(ds->data_likelihood, "TESTBINOMIAL1")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_test_binomial_1;
		ds->data_id = L_TEST_BINOMIAL_1;
	} else if (!strcasecmp(ds->data_likelihood, "CBINOMIAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_cbinomial;
		ds->data_id = L_CBINOMIAL;
	} else if (!strcasecmp(ds->data_likelihood, "GAMMA")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_gamma;
		ds->data_id = L_GAMMA;
	} else if (!strcasecmp(ds->data_likelihood, "GAMMACOUNT")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_gammacount;
		ds->data_id = L_GAMMACOUNT;
	} else if (!strcasecmp(ds->data_likelihood, "BETA")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_beta;
		ds->data_id = L_BETA;
	} else if (!strcasecmp(ds->data_likelihood, "BETABINOMIAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_betabinomial;
		ds->data_id = L_BETABINOMIAL;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBINOMIAL0")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_binomial0;
		ds->data_id = L_ZEROINFLATEDBINOMIAL0;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBINOMIAL1")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_binomial1;
		ds->data_id = L_ZEROINFLATEDBINOMIAL1;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBINOMIAL2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_binomial2;
		ds->data_id = L_ZEROINFLATEDBINOMIAL2;
	} else if (!strcasecmp(ds->data_likelihood, "ZERONINFLATEDBINOMIAL2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zero_n_inflated_binomial2;
		ds->data_id = L_ZERO_N_INFLATEDBINOMIAL2;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBETABINOMIAL0")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_betabinomial0;
		ds->data_id = L_ZEROINFLATEDBETABINOMIAL0;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBETABINOMIAL1")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_betabinomial1;
		ds->data_id = L_ZEROINFLATEDBETABINOMIAL1;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDBETABINOMIAL2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_betabinomial2;
		ds->data_id = L_ZEROINFLATEDBETABINOMIAL2;
	} else if (!strcasecmp(ds->data_likelihood, "NBINOMIAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_negative_binomial;
		ds->data_id = L_NBINOMIAL;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDNBINOMIAL0")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_negative_binomial0;
		ds->data_id = L_ZEROINFLATEDNBINOMIAL0;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDNBINOMIAL1")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_negative_binomial1;
		ds->data_id = L_ZEROINFLATEDNBINOMIAL1;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDNBINOMIAL2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_negative_binomial2;
		ds->data_id = L_ZEROINFLATEDNBINOMIAL2;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDNBINOMIAL1STRATA2")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_negative_binomial1_strata2;
		ds->data_id = L_ZEROINFLATEDNBINOMIAL1STRATA2;
	} else if (!strcasecmp(ds->data_likelihood, "ZEROINFLATEDNBINOMIAL1STRATA3")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_zeroinflated_negative_binomial1_strata3;
		ds->data_id = L_ZEROINFLATEDNBINOMIAL1STRATA3;
	} else if (!strcasecmp(ds->data_likelihood, "STOCHVOL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_stochvol;
		ds->data_id = L_STOCHVOL;
	} else if (!strcasecmp(ds->data_likelihood, "STOCHVOLT")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_stochvol_t;
		ds->data_id = L_STOCHVOL_T;
	} else if (!strcasecmp(ds->data_likelihood, "STOCHVOLNIG")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_stochvol_nig;
		ds->data_id = L_STOCHVOL_NIG;
	} else if (!strcasecmp(ds->data_likelihood, "LOGPERIODOGRAM")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_logperiodogram;
		ds->data_id = L_LOGPERIODOGRAM;
	} else if (!strcasecmp(ds->data_likelihood, "EXPONENTIAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_exp;
		ds->data_id = L_EXPONENTIAL;
	} else if (!strcasecmp(ds->data_likelihood, "WEIBULL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_weibull;
		ds->data_id = L_WEIBULL;
	} else if (!strcasecmp(ds->data_likelihood, "LOGLOGISTIC")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_loglogistic;
		ds->data_id = L_LOGLOGISTIC;
	} else if (!strcasecmp(ds->data_likelihood, "LOGNORMAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_lognormal;
		ds->data_id = L_LOGNORMAL;
	} else if (!strcasecmp(ds->data_likelihood, "WEIBULLCURE")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_weibull_cure;
		ds->data_id = L_WEIBULL_CURE;
	} else if (!strcasecmp(ds->data_likelihood, "CIRCULARNORMAL")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_circular_normal;
		ds->data_id = L_CIRCULAR_NORMAL;
	} else if (!strcasecmp(ds->data_likelihood, "WRAPPEDCAUCHY")) {
		ds->loglikelihood = (GMRFLib_logl_tp *) loglikelihood_wrapped_cauchy;
		ds->data_id = L_WRAPPED_CAUCHY;
	} else {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, "LIKELIHOOD", ds->data_likelihood);
	}
	if (mb->verbose) {
		printf("\t\tlikelihood=[%s]\n", ds->data_likelihood);
	}


	inla_read_fileinfo(mb, ini, sec, &(ds->data_file), NULL);
	inla_read_fileinfo(mb, ini, sec, &(ds->weight_file), "WEIGHTS");
	inla_read_data_likelihood(mb, ini, sec);
	/*
	 * validate the data 
	 */
	switch (ds->data_id) {
	case L_GAUSSIAN:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_gaussian[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Gaussian weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_gaussian[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_SIMPLEX:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_simplex[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Simplex weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_simplex[i]);
					inla_error_general(msg);
				}
				if (ds->data_observations.y[i] <= 0.0 || ds->data_observations.y[i] >= 1) {
					GMRFLib_sprintf(&msg, "%s: Simplex data[%1d] (y) = (%g) is void\n", secname, i, ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_CIRCULAR_NORMAL:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_circular_normal[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Circular Normal weight[%1d] = %g is void\n", secname, i,
							ds->data_observations.weight_circular_normal[i]);
					inla_error_general(msg);
				}
			}
			if (ds->data_observations.d[i]) {
				if (ABS(ds->data_observations.y[i]) > 2.0 * M_PI) {
					GMRFLib_sprintf(&msg, "%s: Circular Normal observation y[%1d] = %g is void\n", secname, i, ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_WRAPPED_CAUCHY:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_wrapped_cauchy[i] <= 0.0 || ds->data_observations.weight_wrapped_cauchy[i] > 1.0) {
					GMRFLib_sprintf(&msg, "%s: Wrapped Cauchy weight[%1d] = %g is void\n", secname, i,
							ds->data_observations.weight_wrapped_cauchy[i]);
					inla_error_general(msg);
				}
			}
			if (ds->data_observations.d[i]) {
				if (ABS(ds->data_observations.y[i]) > 2.0 * M_PI) {
					GMRFLib_sprintf(&msg, "%s: Wrapped Cauchy observation y[%1d] = %g is void\n", secname, i, ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_IID_GAMMA:
	case L_IID_LOGITBETA:
	case L_LOGGAMMA_FRAILTY:
		/*
		 * ok...
		 */
		break;

	case L_LOGISTIC:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_logistic[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Logistic weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_logistic[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_SKEWNORMAL:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_skew_normal[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Skewnormal weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_skew_normal[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_SKEWNORMAL2:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_skew_normal2[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Skewnormal2 weight[%1d] = %g is void\n", secname, i,
							ds->data_observations.weight_skew_normal2[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_GEV:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_gev[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: GEV weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_gev[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_T:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_t[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Student-t weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_t[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_TSTRATA:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_tstrata[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: t weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_tstrata[i]);
					inla_error_general(msg);
				}
				if ((int) (ds->data_observations.strata_tstrata[i]) < 0) {
					GMRFLib_sprintf(&msg, "%s: tstrata strata[%1d] = %g is void\n", secname, i, ds->data_observations.strata_tstrata[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_LAPLACE:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.weight_laplace[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Laplace weight[%1d] = %g is void\n", secname, i, ds->data_observations.weight_laplace[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_POISSON:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.E[i] < 0.0 || ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: Poisson data[%1d] (e,y) = (%g,%g) is void\n", secname, i,
							ds->data_observations.E[i], ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_GAMMACOUNT:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: Gammacount data[%1d] (y) = (%g) is void\n", secname, i, ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_GPOISSON:
	case L_ZEROINFLATEDPOISSON0:
	case L_ZEROINFLATEDPOISSON1:
	case L_ZEROINFLATEDPOISSON2:
	case L_NBINOMIAL:
	case L_ZEROINFLATEDNBINOMIAL0:
	case L_ZEROINFLATEDNBINOMIAL1:
	case L_ZEROINFLATEDNBINOMIAL2:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.E[i] <= 0.0 || ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: Poisson-like data[%1d] (e,y) = (%g,%g) is void\n", secname, i,
							ds->data_observations.E[i], ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_ZEROINFLATEDNBINOMIAL1STRATA2:
	case L_ZEROINFLATEDNBINOMIAL1STRATA3:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.E[i] < 0.0 || ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: Poisson data[%1d] (e,y) = (%g,%g) is void\n", secname, i,
							ds->data_observations.E[i], ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_GAMMA:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.y[i] < 0.0 || ds->data_observations.gamma_weight[i] <= 0.0) {
					GMRFLib_sprintf(&msg, "%s: Gamma data[%1d] (y) = %g or weight %g is void\n", secname, i,
							ds->data_observations.y[i], ds->data_observations.gamma_weight[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_BETA:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.y[i] <= 0.0 || ds->data_observations.y[i] >= 1.0) {
					GMRFLib_sprintf(&msg, "%s: Beta data[%1d] (y) = (%g) is void\n", secname, i, ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_BINOMIAL:
	case L_ZERO_N_INFLATEDBINOMIAL2:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.nb[i] <= 0.0 ||
				    ds->data_observations.y[i] > ds->data_observations.nb[i] || ds->data_observations.y[i] < 0.0) {
					// we allow for binomial(0,p) if y = 0
					if (!(ISZERO(ds->data_observations.nb[i]) && ISZERO(ds->data_observations.y[i]))) {
						GMRFLib_sprintf(&msg, "%s: Binomial data[%1d] (nb,y) = (%g,%g) is void\n", secname,
								i, ds->data_observations.nb[i], ds->data_observations.y[i]);
						inla_error_general(msg);
					}
				}
			}
		}
		break;

	case L_ZEROINFLATEDBINOMIAL0:
	case L_ZEROINFLATEDBINOMIAL1:
	case L_ZEROINFLATEDBINOMIAL2:
	case L_ZEROINFLATEDBETABINOMIAL0:
	case L_ZEROINFLATEDBETABINOMIAL1:
	case L_ZEROINFLATEDBETABINOMIAL2:
	case L_BETABINOMIAL:
	case L_TEST_BINOMIAL_1:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.nb[i] <= 0.0 ||
				    ds->data_observations.y[i] > ds->data_observations.nb[i] || ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: Binomial data[%1d] (nb,y) = (%g,%g) is void\n", secname,
							i, ds->data_observations.nb[i], ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_CBINOMIAL:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				if (ds->data_observations.cbinomial_n[i] <= 0.0 ||
				    ds->data_observations.cbinomial_k[i] <= 0.0 ||
				    ds->data_observations.y[i] > ds->data_observations.cbinomial_k[i] || ds->data_observations.y[i] < 0.0) {
					GMRFLib_sprintf(&msg, "%s: CBinomial data[%1d] (k,n,y) = (%g,%g,%g) is void\n", secname,
							i, ds->data_observations.cbinomial_k[i], ds->data_observations.cbinomial_n[i], ds->data_observations.y[i]);
					inla_error_general(msg);
				}
			}
		}
		break;

	case L_EXPONENTIAL:
	case L_WEIBULL:
	case L_WEIBULL_CURE:
	case L_LOGLOGISTIC:
	case L_LOGNORMAL:
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (ds->data_observations.d[i]) {
				int event;
				double truncation, lower, upper, ttime;

				truncation = ds->data_observations.truncation[i];
				lower = ds->data_observations.lower[i];
				upper = ds->data_observations.upper[i];
				ttime = ds->data_observations.y[i];
				event = (int) (ds->data_observations.event[i]);

#define SERR { GMRFLib_sprintf(&msg, "%s: ps survival data[%1d] (event,trunc,lower,upper,y) = (%g,%g,%g,%g,%g) is void\n", \
			       secname, i, (double)event, truncation, lower,  upper,  ttime); inla_error_general(msg); }

				if (truncation < 0.0 || lower < 0.0 || upper < 0.0 || ttime < 0.0)
					SERR;

				switch (event) {
				case SURV_EVENT_FAILURE:
					if (ttime < truncation)
						SERR;
					break;
				case SURV_EVENT_RIGHT:
					if (lower < truncation)
						SERR;
					break;
				case SURV_EVENT_LEFT:
					if (upper < truncation)
						SERR;
					break;
				case SURV_EVENT_INTERVAL:
					if (DMIN(lower, upper) < truncation || upper < lower)
						SERR;
					break;
				default:
					SERR;
					break;
				}
#undef SERR
			}
		}
		break;

	default:
		break;
	}

	/*
	 * common for all 
	 */
	ds->variant = iniparser_getint(ini, inla_string_join(secname, "VARIANT"), 0);
	if (mb->verbose) {
		printf("\t\tlikelihood.variant=[%1d]\n", ds->variant);
	}

	/*
	 * read spesific options and define hyperparameters, if any.
	 */
	switch (ds->data_id) {
	case L_GAUSSIAN:
		/*
		 * get options related to the gaussian 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_gaussian, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_gaussian[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log precision for the Gaussian observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision for the Gaussian observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_gaussian;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_SIMPLEX:
		/*
		 * get options related to the gaussian 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_simplex, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_prec[%g]\n", ds->data_observations.log_prec_simplex[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log precision for the Simplex observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision for the Simplex observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_simplex;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_GPOISSON:
		/*
		 * get options related to the gpoisson 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), -5.0);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.gpoisson_overdispersion, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_overdispersion[%g]\n", ds->data_observations.gpoisson_overdispersion[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log overdispersion for gpoisson", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Overdispersion for gpoisson", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.gpoisson_overdispersion;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the 'p' parameter
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 1);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.gpoisson_p, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise p[%g]\n", ds->data_observations.gpoisson_p[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "normal");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Parameter p for gpoisson", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Parameter p for gpoisson", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.gpoisson_p;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_CIRCULAR_NORMAL:
		/*
		 * get options related to the circular normal
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_circular_normal, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision parameter[%g]\n", ds->data_observations.log_prec_circular_normal[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log precision parameter for the Circular Normal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision parameter for the Circular Normal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_circular_normal;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_WRAPPED_CAUCHY:
		/*
		 * get options related to the circular cauchy
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_wrapped_cauchy, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision parameter[%g]\n", ds->data_observations.log_prec_wrapped_cauchy[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log precision parameter for the Wrapped Cauchy observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision parameter for the Wrapped Cauchy observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_wrapped_cauchy;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_TEST_BINOMIAL_1:
		/*
		 * get options related to the test-binomial-1: sensitivity 's'
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* yes! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.test_binomial_1_s, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise logit sensitivity[%g]\n", ds->data_observations.test_binomial_1_s[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "NORMAL");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Logit sensitivity for test-binomial-1", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Sensitivity for test-binomial-1", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.test_binomial_1_s;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the 'specificity' parameter 'e'
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.test_binomial_1_e, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise logit specificity[%g]\n", ds->data_observations.test_binomial_1_e[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "NORMAL");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Logit specificity for test-binomial-1", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Specificity for test-binomial-1", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.test_binomial_1_e;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_IID_GAMMA:
		/*
		 * get options related to the iid_gamma
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* yes! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.iid_gamma_log_shape, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_shape[%g]\n", ds->data_observations.iid_gamma_log_shape[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log shape parameter for iid-gamma", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Shape parameter for iid-gamma", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.iid_gamma_log_shape;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the 'rate' parameter
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.iid_gamma_log_rate, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_rate[%g]\n", ds->data_observations.iid_gamma_log_rate[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "loggamma");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log rate parameter for iid-gamma", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Rate parameter for iid-gamma", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.iid_gamma_log_rate;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_IID_LOGITBETA:
		/*
		 * get options related to the iid_logitbeta. first log(a)
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* yes! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.iid_logitbeta_log_a, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_a[%g]\n", ds->data_observations.iid_logitbeta_log_a[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log a parameter for iid-beta", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("a parameter for iid-beta", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.iid_logitbeta_log_a;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * log(b)
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.iid_logitbeta_log_b, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_b[%g]\n", ds->data_observations.iid_logitbeta_log_b[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "loggamma");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log b parameter for iid-beta", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("b parameter for iid-beta", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.iid_logitbeta_log_b;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_LOGGAMMA_FRAILTY:
		/*
		 * get options related to the loggammafrailty
		 */

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_loggamma_frailty, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_loggamma_frailty[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for the gamma frailty", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for the gamma frailty", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_loggamma_frailty;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_LOGNORMAL:
		/*
		 * get options related to the lognormal 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_gaussian, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_gaussian[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for the lognormal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for the lognormal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_gaussian;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_LOGISTIC:
		/*
		 * get options related to the logistic 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_logistic, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_logistic[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for the logistic observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for the logistic observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_logistic;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_SKEWNORMAL:
		/*
		 * get options related to the skew-normal
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* yes! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_skew_normal, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_skew_normal[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for skew-normal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for skew-normal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_skew_normal;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the 'shape' parameter/ the skewness parameter
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "SN.SHAPE.MAX"), 5.0);
		ds->data_observations.shape_max_skew_normal = iniparser_getdouble(ini, inla_string_join(secname, "SNSHAPEMAX"), tmp);
		ds->data_observations.shape_max_skew_normal = ABS(ds->data_observations.shape_max_skew_normal);
		if (mb->verbose) {
			printf("\t\tshape.max[%g]\n", ds->data_observations.shape_max_skew_normal);
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.shape_skew_normal, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise shape[%g]\n", ds->data_observations.shape_skew_normal[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern shape parameter for skew-normal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("shape parameter for skew-normal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.shape_skew_normal;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_rho;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_SKEWNORMAL2:
		/*
		 * get options related to the skew-normal
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* yes! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_skew_normal2, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_skew_normal2[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for skew-normal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for skew-normal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_skew_normal2;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the standarized skewness
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.logit_skewness_skew_normal2, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise intern skewness[%g]\n", ds->data_observations.logit_skewness_skew_normal2[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern skewness for skew-normal observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("skewness for skew-normal observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.logit_skewness_skew_normal2;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_rho;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_GEV:
		/*
		 * get options related to the gev
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "gev.scale.xi"), 0.01);	/* yes! */
		assert(tmp > 0.0);
		ds->data_observations.gev_scale_xi = tmp;
		if (mb->verbose) {
			printf("\t\tgev.scale.xi [%g]\n", ds->data_observations.gev_scale_xi);
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);	/* YES! */
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_gev, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_gev[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for GEV observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for GEV observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_gev;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the 'xi' parameter/ the gev-parameter. we need a little care, as the user see 'xi' while we work internally with 'xi/xi.scale'
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0) / ds->data_observations.gev_scale_xi;	/* scale here */
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			// tmp = mb->theta_file[mb->theta_counter_file++]/ds->data_observations.gev_scale_xi; /* scale here */
			tmp = mb->theta_file[mb->theta_counter_file++];	/* DO NOT scale here */
		}
		HYPER_NEW(ds->data_observations.xi_gev, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise gev-parameter[%g] (scaled with scale.xi=[%g])\n", ds->data_observations.xi_gev[0][0],
			       ds->data_observations.gev_scale_xi);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-a");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern shape-parameter for gev-observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("shape-parameter for gev observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.xi_gev;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity_scale;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = (void *) &(ds->data_observations.gev_scale_xi);
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_GAMMA:
		/*
		 * get options related to the gamma
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 0.0);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.gamma_log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise precision_intern[%g]\n", ds->data_observations.gamma_log_prec[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Intern precision-parameter for the Gamma observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision parameter for the Gamma observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.gamma_log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_GAMMACOUNT:
		/*
		 * get options related to the gammacount
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 0.0);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.gammacount_log_alpha, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_alpha[%g]\n", ds->data_observations.gammacount_log_alpha[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log-alpha parameter for Gammacount observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Alpha parameter for Gammacount observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.gammacount_log_alpha;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_BETA:
		/*
		 * get options related to the beta
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 0.0);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.beta_precision_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise precision_intern[%g]\n", ds->data_observations.beta_precision_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern precision-parameter for the beta observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision parameter for the beta observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.beta_precision_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_BETABINOMIAL:
		/*
		 * get options related to the betabinomial
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 0.0);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.betabinomial_overdispersion_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise overdispersion_intern[%g]\n", ds->data_observations.betabinomial_overdispersion_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern overdispersion for the betabinomial observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("overdispersion for the betabinomial observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.betabinomial_overdispersion_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_NBINOMIAL:
		/*
		 * get options related to the negative binomial
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), log(10.0));
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_size, tmp);
		assert(ds->variant == 0 || ds->variant == 1);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[%g]\n", ds->data_observations.log_size[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
			printf("\t\tuse parameterization variant=[%1d]; see doc for details\n", ds->variant);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size for the nbinomial observations (overdispersion)", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size for the nbinomial observations (overdispersion)", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_size;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_ZEROINFLATEDNBINOMIAL0:
	case L_ZEROINFLATEDNBINOMIAL1:
		/*
		 * get options related to the zeroinflated negative binomial_0/1
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), log(10.0));
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_size, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[%g]\n", ds->data_observations.log_size[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size for nbinomial zero-inflated observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size for nbinomial zero-inflated observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_size;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), -1.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob_intern[%g]\n", ds->data_observations.prob_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			if (ds->data_id == L_ZEROINFLATEDNBINOMIAL0) {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated nbinomial_0", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated nbinomial_0", mb->ds);
			} else {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated nbinomial_1", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated nbinomial_1", mb->ds);
			}
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_ZEROINFLATEDBETABINOMIAL0:
	case L_ZEROINFLATEDBETABINOMIAL1:
		/*
		 * get options related to the zeroinflated beta binomial_0/1
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), log(10.0));
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_rho_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise rho_intern[%g]\n", ds->data_observations.zeroinflated_rho_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("rho_intern for zero-inflated betabinomial", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("rho for zero-inflated betabinomial", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_rho_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), -1.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob_intern[%g]\n", ds->data_observations.prob_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			if (ds->data_id == L_ZEROINFLATEDBETABINOMIAL0) {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated betabinomial_0", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated betabinomial_0", mb->ds);
			} else {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated betabinomial_1", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated betabinomial_1", mb->ds);
			}
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;


	case L_ZEROINFLATEDNBINOMIAL1STRATA2:
		/*
		 * get options related to the zeroinflated negative binomial_0/1, strata2
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), log(10.0));
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_size, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[%g]\n", ds->data_observations.log_size[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size for nbinomial zero-inflated observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size for nbinomial zero-inflated observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_size;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter 1
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), -1.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob1_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob1_intern[%g]\n", ds->data_observations.prob1_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter1 for zero-inflated nbinomial_1_strata2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter1 for zero-inflated nbinomial_1_strata2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob1_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter 2
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), -1.0);
		ds->data_fixed2 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED2"), 0);
		if (!ds->data_fixed2 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob2_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob2_intern[%g]\n", ds->data_observations.prob2_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed2);
		}
		inla_read_prior2(mb, ini, sec, &(ds->data_prior2), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed2) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter2 for zero-inflated nbinomial1_strata2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter2 for zero-inflated nbinomial1_strata2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter2", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob2_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_ZEROINFLATEDNBINOMIAL1STRATA3:
		/*
		 * get options related to the zeroinflated negative binomial1, strata3
		 */
		ds->data_observations.log_sizes = Calloc(2, double **);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), log(10.0));
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_sizes[0], tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[0][%g]\n", ds->data_observations.log_sizes[0][0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size1 for nbinomial zero-inflated observations strata3", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size1 for nbinomial zero-inflated observations strata3", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_sizes[0];
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), log(10.0));
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_sizes[1], tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[1][%g]\n", ds->data_observations.log_sizes[1][0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size2 for nbinomial zero-inflated observations strata3", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size2 for nbinomial zero-inflated observations strata3", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_sizes[1];
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), -1.0);
		ds->data_fixed2 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED2"), 0);
		if (!ds->data_fixed2 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob_intern[%g]\n", ds->data_observations.prob_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed2);
		}
		inla_read_prior2(mb, ini, sec, &(ds->data_prior2), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed2) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated nbinomial 1 strata3", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated nbinomial 1 strata3", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter2", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior2.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior2.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_ZEROINFLATEDNBINOMIAL2:
		/*
		 * get options related to the zeroinflated negative binomial_2
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), log(10.0));
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_size, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_size[%g]\n", ds->data_observations.log_size[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");


		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log size for nbinomial zero-inflated observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("size for nbinomial zero-inflated observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_size;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		/*
		 * the zeroinflation parameter; the parameter alpha (see the documentation) 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), log(2.0));
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.zeroinflated_alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "gaussian-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("parameter alpha.intern for zero-inflated nbinomial2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("parameter alpha for zero-inflated nbinomial2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_LAPLACE:
		/*
		 * get options related to the laplace. the specials are alpha, determining the asymmetry, and epsilon, determine the area which is approximated with
		 * a gaussian
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_tau_laplace, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_tau[%g]\n", ds->data_observations.log_tau_laplace[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		ds->data_observations.alpha_laplace = iniparser_getdouble(ini, inla_string_join(secname, "alpha"), 0.5);
		ds->data_observations.epsilon_laplace = iniparser_getdouble(ini, inla_string_join(secname, "epsilon"), 0.01);
		ds->data_observations.gamma_laplace = iniparser_getdouble(ini, inla_string_join(secname, "gamma"), 1.0);
		if (mb->verbose) {
			printf("\t\talpha=[%g]\n", ds->data_observations.alpha_laplace);
			printf("\t\tepsilon=[%g]\n", ds->data_observations.epsilon_laplace);
			printf("\t\tgamma=[%g]\n", ds->data_observations.gamma_laplace);
		}

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log tau for laplace observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("tau for laplace observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_tau_laplace;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_tau_laplace;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_T:
		/*
		 * get options related to the t
		 */

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_prec_t, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", ds->data_observations.log_prec_t[0][0]);
			printf("\t\tfixed0=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA");

		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("log precision for the student-t observations", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("precision for the student-t observations", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_prec_t;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		/*
		 * dof 
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 3.0);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.dof_intern_t, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise dof_intern_t[%g]\n", ds->data_observations.dof_intern_t[0][0]);
			printf("\t\tfixed1=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "normal");

		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("dof_intern for student-t", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("degrees of freedom for student-t", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.dof_intern_t;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_dof;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;

	case L_TSTRATA:
	{
		/*
		 * get options related to the tstrata
		 */
		int k;

		ds->data_nprior = Calloc(TSTRATA_MAXTHETA, Prior_tp);
		ds->data_nfixed = Calloc(TSTRATA_MAXTHETA, int);

		/*
		 * check how many strata we have 
		 */
		int nstrata = 0;
		for (k = 0; k < mb->predictor_ndata; k++) {
			if (ds->data_observations.d[k]) {
				nstrata = IMAX(nstrata, (int) ds->data_observations.strata_tstrata[k]);
			}
		}
		nstrata++;
		assert(nstrata <= TSTRATA_MAXTHETA - 1);

		/*
		 * dof = theta0
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 3.0);
		ds->data_nfixed[0] = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_nfixed[0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.dof_intern_tstrata, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise dof_intern_tstrata[%g]\n", ds->data_observations.dof_intern_tstrata[0][0]);
			printf("\t\tfixed0=[%1d]\n", ds->data_nfixed[0]);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_nprior[0]), "normal");

		if (!ds->data_nfixed[0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("dof_intern for tstrata", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("degrees of freedom for tstrata", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_nprior[0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_nprior[0].to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.dof_intern_tstrata;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_dof5;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		ds->data_observations.log_prec_tstrata = Calloc(TSTRATA_MAXTHETA - 1, double **);

		for (k = 1; k < TSTRATA_MAXTHETA; k++) {

			char *ctmp = NULL, *pri = NULL, *par = NULL, *from_theta = NULL, *to_theta = NULL;

			GMRFLib_sprintf(&pri, "PRIOR%1d", k);
			GMRFLib_sprintf(&par, "PARAMETERS%1d", k);
			GMRFLib_sprintf(&from_theta, "FROM.THETA%1d", k);
			GMRFLib_sprintf(&to_theta, "TO.THETA%1d", k);
			inla_read_prior_generic(mb, ini, sec, &(ds->data_nprior[k]), pri, par, from_theta, to_theta, "normal");

			GMRFLib_sprintf(&ctmp, "FIXED%1d", k);
			ds->data_nfixed[k] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);
			/*
			 * if above number of stata, then its fixed for sure! 
			 */
			if (k > nstrata) {
				ds->data_nfixed[k] = 1;
			}
			GMRFLib_sprintf(&ctmp, "INITIAL%1d", k);
			double initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), G.log_prec_initial);

			if (!ds->data_nfixed[k] && mb->reuse_mode) {
				initial = mb->theta_file[mb->theta_counter_file++];
			}
			HYPER_NEW(ds->data_observations.log_prec_tstrata[k - 1], initial);	/* yes, its a -1, prec0, prec1, etc... */
			if (mb->verbose) {
				printf("\t\tinitialise log_prec_tstrata[%1d][%g]\n", ds->data_nfixed[k], ds->data_observations.log_prec_tstrata[k - 1][0][0]);
				printf("\t\tfixed%1d=[%1d]\n", k, ds->data_nfixed[k]);
			}

			if (!ds->data_nfixed[k]) {

				mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
				mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
				mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
				mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

				mb->theta_tag[mb->ntheta] = inla_make_tag("Log prec for tstrata strata", k - 1);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Prec tstrata strata", k - 1);
				GMRFLib_sprintf(&msg, "%s-parameter%1d", secname, k);
				mb->theta_dir[mb->ntheta] = msg;

				mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
				mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
				mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_nprior[k].from_theta);
				mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_nprior[k].to_theta);

				mb->theta[mb->ntheta] = ds->data_observations.log_prec_tstrata[k - 1];	/* yes its a -1 */
				mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
				mb->theta_map[mb->ntheta] = map_precision;
				mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
				mb->theta_map_arg[mb->ntheta] = NULL;
				mb->ntheta++;
				ds->data_ntheta++;
			}

			Free(pri);
			Free(par);
			Free(from_theta);
			Free(to_theta);
			Free(ctmp);
		}
		break;
	}

	case L_STOCHVOL:
	{
		/*
		 * get options related to the stochvol; the log-offset in the variance
		 */
		double initial_value = 500.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 1);	/* yes, default fixed */
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.log_offset_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log.offset[%g]\n", ds->data_observations.log_offset_prec[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Log offset precision for stochvol", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Offset precision for stochvol", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.log_offset_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_STOCHVOL_T:
	{
		/*
		 * get options related to the stochvol_t
		 */
		double initial_value = 3.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.dof_intern_svt, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise dof_intern[%g]\n", ds->data_observations.dof_intern_svt[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "GAUSSIAN");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("dof_intern for stochvol student-t", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("degrees of freedom for stochvol student-t", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.dof_intern_svt;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_dof;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_STOCHVOL_NIG:
	{
		/*
		 * get options related to the stochvol_nig
		 *
		 * first parameter is skew, second is shape
		 */
		double initial0 = 0.0, initial1 = 1.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), initial0);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.skew_intern_svnig, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise skew_intern_svnig[%g]\n", ds->data_observations.skew_intern_svnig[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "GAUSSIAN");

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), initial1);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.shape_intern_svnig, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise shape_intern_snvig[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("skewness_param_intern for stochvol-nig", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("skewness parameter for stochvol-nig", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.skew_intern_svnig;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("shape_param_intern for stochvol-nig", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("shape parameter for stochvol-nig", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.shape_intern_svnig;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_shape_svnig;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		ds->data_ntheta = (ds->data_fixed0 ? 0 : 1) + (ds->data_fixed1 ? 0 : 1);
		break;
	}

	case L_WEIBULL:
	{
		/*
		 * get options related to the weibull
		 */
		double initial_value = 0.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA-ALPHA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("alpha_intern for weibull", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("alpha parameter for weibull", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_alpha_weibull;	/* alpha = exp(alpha.intern) */
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_LOGLOGISTIC:
	{
		/*
		 * get options related to the loglogistic
		 */
		double initial_value = 0.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "LOGGAMMA-ALPHA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("alpha_intern for loglogistic", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("alpha parameter for loglogistic", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_alpha_loglogistic;	/* alpha = exp(alpha.intern) */
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_WEIBULL_CURE:
	{
		/*
		 * get options related to the ps
		 */
		double initial_value = 0.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), initial_value);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "LOGGAMMA-ALPHA");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("alpha_intern for ps", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("alpha parameter for ps", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_alpha_weibull_cure;	/* alpha = exp(alpha.intern) */
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		initial_value = 1.0;
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), initial_value);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.p_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise p_intern[%g]\n", ds->data_observations.p_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add p
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("p.intern for ps", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("p parameter for ps", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.p_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_p_weibull_cure;	/* p = exp(p.intern)/(1+exp(p.intern)) */
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZEROINFLATEDPOISSON0:
	case L_ZEROINFLATEDPOISSON1:
	{
		/*
		 * get options related to the zeroinflatedpoisson0/1
		 */
		double initial_value = -1.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob_intern[%g]\n", ds->data_observations.prob_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			if (ds->data_id == L_ZEROINFLATEDPOISSON0) {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated poisson_0", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated poisson_0", mb->ds);
			} else {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated poisson_1", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated poisson_1", mb->ds);
			}
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZEROINFLATEDPOISSON2:
	{
		/*
		 * get options related to the zeroinflatedpoisson2
		 */
		double initial_value = log(2.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.zeroinflated_alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "GAUSSIAN-std");
		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated poisson_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated poisson_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZEROINFLATEDBINOMIAL2:
	{
		/*
		 * get options related to the zeroinflatedbinomial2
		 */
		double initial_value = log(2.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.zeroinflated_alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated binomial_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated binomial_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZERO_N_INFLATEDBINOMIAL2:
	{
		/*
		 * get options related to the zero_n_inflatedbinomial2
		 */
		double initial_value = log(2.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), initial_value);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zero_n_inflated_alpha1_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha1_intern[%g]\n", ds->data_observations.zero_n_inflated_alpha1_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern alpha1 parameter for zero-n-inflated binomial_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("alpha1 parameter for zero-n-inflated binomial_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zero_n_inflated_alpha1_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), initial_value);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zero_n_inflated_alpha2_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha2_intern[%g]\n", ds->data_observations.zero_n_inflated_alpha2_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern alpha2 parameter for zero-n-inflated binomial_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("alpha2 parameter for zero-n-inflated binomial_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zero_n_inflated_alpha2_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZEROINFLATEDBETABINOMIAL2:
	{
		/*
		 * get options related to the zeroinflatedbetabinomial2
		 */
		double initial_value = log(2.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), initial_value);
		ds->data_fixed0 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED0"), 0);
		if (!ds->data_fixed0 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_alpha_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise alpha_intern[%g]\n", ds->data_observations.zeroinflated_alpha_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed0);
		}
		inla_read_prior0(mb, ini, sec, &(ds->data_prior0), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed0) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated betabinomial_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated betabinomial_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior0.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_alpha_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}

		initial_value = log(1.0);
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), initial_value);
		ds->data_fixed1 = iniparser_getboolean(ini, inla_string_join(secname, "FIXED1"), 0);
		if (!ds->data_fixed1 && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.zeroinflated_delta_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise delta_intern[%g]\n", ds->data_observations.zeroinflated_delta_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed1);
		}
		inla_read_prior1(mb, ini, sec, &(ds->data_prior1), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed1) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("intern overdispersion parameter for zero-inflated betabinomial_2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("overdispersion parameter for zero-inflated betabinomial_2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter1", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior1.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.zeroinflated_delta_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	case L_ZEROINFLATEDBINOMIAL0:
	case L_ZEROINFLATEDBINOMIAL1:
	{
		/*
		 * get options related to the zeroinflatedbinomial0/1
		 */
		double initial_value = -1.0;

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), initial_value);
		ds->data_fixed = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
		if (!ds->data_fixed && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->data_observations.prob_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise prob_intern[%g]\n", ds->data_observations.prob_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->data_fixed);
		}
		inla_read_prior(mb, ini, sec, &(ds->data_prior), "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->data_fixed) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			if (ds->data_id == L_ZEROINFLATEDBINOMIAL0) {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated binomial_0", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated binomial_0", mb->ds);
			} else {
				mb->theta_tag[mb->ntheta] = inla_make_tag("intern zero-probability parameter for zero-inflated binomial_1", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("zero-probability parameter for zero-inflated binomial_1", mb->ds);
			}
			GMRFLib_sprintf(&msg, "%s-parameter", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->data_prior.from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->data_prior.to_theta);

			mb->theta[mb->ntheta] = ds->data_observations.prob_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->data_ntheta++;
		}
		break;
	}

	default:
		/*
		 * nothing to do 
		 */
		ds->data_ntheta = 0;
	}


	/*
	 * setup the link-model
	 */
	ds->link_model = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join(secname, "LINK.MODEL"), GMRFLib_strdup("default"))));
	inla_trim_family(ds->link_model);

	if (!strcasecmp(ds->link_model, "IDENTITY")) {
		ds->link_id = LINK_IDENTITY;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_identity;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "LOG")) {
		ds->link_id = LINK_LOG;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_log;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "PROBIT")) {
		ds->link_id = LINK_PROBIT;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_probit;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "CLOGLOG")) {
		ds->link_id = LINK_CLOGLOG;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_cloglog;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "LOGIT")) {
		ds->link_id = LINK_LOGIT;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_logit;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "TAN")) {
		ds->link_id = LINK_TAN;
		ds->link_ntheta = 0;
		ds->predictor_invlinkfunc = link_tan;
		ds->predictor_invlinkfunc_arg = NULL;
	} else if (!strcasecmp(ds->link_model, "LOGOFFSET")) {
		ds->link_id = LINK_LOGOFFSET;
		ds->link_ntheta = 1;
		ds->predictor_invlinkfunc = link_logoffset;
		ds->predictor_invlinkfunc_arg = NULL;	       /* to be completed */
	} else if (!strcasecmp(ds->link_model, "SSLOGIT")) {
		ds->link_id = LINK_SSLOGIT;
		ds->link_ntheta = 2;
		ds->predictor_invlinkfunc = link_sslogit;
		ds->predictor_invlinkfunc_arg = NULL;	       /* to be completed */
	} else if (!strcasecmp(ds->link_model, "TEST1")) {
		ds->link_id = LINK_TEST1;
		ds->link_ntheta = 1;
		ds->predictor_invlinkfunc = link_test1;
		ds->predictor_invlinkfunc_arg = NULL;	       /* to be completed */
	} else if (!strcasecmp(ds->link_model, "SPECIAL1")) {
		ds->link_id = LINK_SPECIAL1;
		ds->link_ntheta = -1;
		ds->predictor_invlinkfunc = link_special1;
		ds->predictor_invlinkfunc_arg = NULL;	       /* to be completed */
	} else if (!strcasecmp(ds->link_model, "SPECIAL2")) {
		ds->link_id = LINK_SPECIAL2;
		ds->link_ntheta = 1;
		ds->predictor_invlinkfunc = link_special2;
		ds->predictor_invlinkfunc_arg = NULL;	       /* to be completed */
	} else {
		char *msg;
		GMRFLib_sprintf(&msg, "Unknown link-model [%s]\n", ds->link_model);
		inla_error_general(msg);
		exit(1);
	}

	/*
	 * the meaning of 'order' is model dependent
	 */
	ds->link_order = iniparser_getint(ini, inla_string_join(secname, "LINK.ORDER"), -1);
	if (mb->verbose) {
		printf("\t\tLink model  [%s]\n", ds->link_model);
		printf("\t\tLink order  [%1d]\n", ds->link_order);
		printf("\t\tLink ntheta [%1d]\n", ds->link_ntheta);
	}

	/*
	 * read possible link_covariates 
	 */
	char *link_cov_filename;
	link_cov_filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "LINK.COVARIATES"), NULL));

	if (link_cov_filename) {
		ds->link_covariates = GMRFLib_read_fmesher_file(link_cov_filename, (long int) 0, -1);
		if (mb->verbose) {
			int ii, jj;
			printf("\t\tLink_covariates: file[%s] dim=(%1d x %1d)\n", link_cov_filename, ds->link_covariates->nrow, ds->link_covariates->ncol);
			for (ii = 0; ii < IMIN(PREVIEW, ds->link_covariates->nrow); ii++) {
				printf("\t\t\trow=%1d, covariates=[", ii);
				for (jj = 0; jj < ds->link_covariates->ncol; jj++) {
					printf(" %g", GMRFLib_matrix_get(ii, jj, ds->link_covariates));
				}
				printf(" ]\n");
			}
		}

		int len = (mb->predictor_m > 0 ? mb->predictor_m : mb->predictor_n);
		if (len != ds->link_covariates->nrow) {
			char *emsg;
			GMRFLib_sprintf(&emsg,
					"link.covariates has not the same number of rows as the linear predictor %1d != %1d", ds->link_covariates->nrow, len);
			inla_error_general(emsg);
			abort();
		}
	} else {
		ds->link_covariates = NULL;
	}

	/*
	 * read link-parameters, if any
	 */
	switch (ds->link_id) {
	case LINK_IDENTITY:
	case LINK_LOG:
	case LINK_PROBIT:
	case LINK_CLOGLOG:
	case LINK_LOGIT:
	case LINK_TAN:
		/*
		 * no parameters
		 */
		break;

	case LINK_SSLOGIT:
		/*
		 * logit link with correction for sensitivity and specificity
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "LINK.INITIAL0"), 0.0);
		ds->link_fixed = Calloc(2, int);
		ds->link_fixed[0] = iniparser_getboolean(ini, inla_string_join(secname, "LINK.FIXED0"), 0);
		if (!ds->link_fixed[0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		ds->link_parameters = Calloc(1, Link_param_tp);
		ds->link_parameters->order = -1;
		ds->predictor_invlinkfunc_arg = (void *) (ds->link_parameters);

		HYPER_NEW(ds->link_parameters->sensitivity_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise sslogit link sensitivity_intern[%g]\n", ds->link_parameters->sensitivity_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->link_fixed[0]);
		}
		ds->link_prior = Calloc(2, Prior_tp);
		inla_read_prior_link0(mb, ini, sec, &(ds->link_prior[0]), "LOGITBETA");	/* read both priors here */
		inla_read_prior_link1(mb, ini, sec, &(ds->link_prior[1]), "LOGITBETA");	/* ... */

		/*
		 * add theta 
		 */
		if (!ds->link_fixed[0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Link sslogit sensitivity_intern", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Link sslogit sensitivity", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].to_theta);
			mb->theta[mb->ntheta] = ds->link_parameters->sensitivity_intern;

			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->link_ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "LINK.INITIAL1"), 0.0);
		ds->link_fixed[1] = iniparser_getboolean(ini, inla_string_join(secname, "LINK.FIXED1"), 0);
		if (!ds->link_fixed[1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		HYPER_NEW(ds->link_parameters->specificity_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise sslogit link specificity_intern[%g]\n", ds->link_parameters->specificity_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->link_fixed[1]);
		}
		if (!ds->link_fixed[1]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Link sslogit specificity_intern", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Link sslogit specificity", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[1].to_theta);
			mb->theta[mb->ntheta] = ds->link_parameters->specificity_intern;

			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->link_ntheta++;
		}
		break;

	case LINK_LOGOFFSET:
		/*
		 * exp(beta)*cov + exp(linear.predictor)
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "LINK.INITIAL"), 0.0);
		ds->link_fixed = Calloc(1, int);
		ds->link_fixed[0] = iniparser_getboolean(ini, inla_string_join(secname, "LINK.FIXED"), 0);
		if (!ds->link_fixed[0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		ds->link_parameters = Calloc(1, Link_param_tp);
		ds->link_parameters->order = ds->link_order;
		ds->predictor_invlinkfunc_arg = (void *) (ds->link_parameters);

		HYPER_NEW(ds->link_parameters->beta_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise link beta[%g]\n", ds->link_parameters->beta_intern[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->link_fixed[0]);
		}
		ds->link_prior = Calloc(1, Prior_tp);
		inla_read_prior_link(mb, ini, sec, ds->link_prior, "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->link_fixed[0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Link beta_intern for logoffset", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Link beta for logoffset", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].to_theta);
			mb->theta[mb->ntheta] = ds->link_parameters->beta_intern;

			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->link_ntheta++;
		}
		break;


	case LINK_TEST1:
		/*
		 * exp(eta - beta*cov)
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "LINK.INITIAL"), 0.0);
		ds->link_fixed = Calloc(1, int);
		ds->link_fixed[0] = iniparser_getboolean(ini, inla_string_join(secname, "LINK.FIXED"), 0);
		if (!ds->link_fixed[0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		ds->link_parameters = Calloc(1, Link_param_tp);
		ds->link_parameters->order = ds->link_order;
		ds->predictor_invlinkfunc_arg = (void *) (ds->link_parameters);

		HYPER_NEW(ds->link_parameters->beta, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise link beta[%g]\n", ds->link_parameters->beta[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->link_fixed[0]);
		}
		ds->link_prior = Calloc(1, Prior_tp);
		inla_read_prior_link(mb, ini, sec, ds->link_prior, "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->link_fixed[0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Link beta for test1", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Link beta for test1", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].to_theta);
			mb->theta[mb->ntheta] = ds->link_parameters->beta;

			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->link_ntheta++;
		}
		break;

	case LINK_SPECIAL2:
		/*
		 * exp(eta) * ( 1-x + x*exp(beta) )
		 */
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "LINK.INITIAL"), 0.0);
		ds->link_fixed = Calloc(1, int);
		ds->link_fixed[0] = iniparser_getboolean(ini, inla_string_join(secname, "LINK.FIXED"), 0);
		if (!ds->link_fixed[0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		ds->link_parameters = Calloc(1, Link_param_tp);
		ds->link_parameters->order = ds->link_order;
		ds->predictor_invlinkfunc_arg = (void *) (ds->link_parameters);

		HYPER_NEW(ds->link_parameters->beta, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise link beta[%g]\n", ds->link_parameters->beta[0][0]);
			printf("\t\tfixed=[%1d]\n", ds->link_fixed[0]);
		}
		ds->link_prior = Calloc(1, Prior_tp);
		inla_read_prior_link(mb, ini, sec, ds->link_prior, "GAUSSIAN-std");

		/*
		 * add theta 
		 */
		if (!ds->link_fixed[0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			mb->theta_tag[mb->ntheta] = inla_make_tag("Link beta for special2", mb->ds);
			mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Link beta for special2", mb->ds);
			GMRFLib_sprintf(&msg, "%s-parameter0", secname);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].to_theta);
			mb->theta[mb->ntheta] = ds->link_parameters->beta;

			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
			ds->link_ntheta++;
		}
		break;

	case LINK_SPECIAL1:
		ds->link_prior = Calloc(2, Prior_tp);
		inla_read_prior_link0(mb, ini, sec, &(ds->link_prior[0]), "LOGGAMMA");	// log precision
		inla_read_prior_link1(mb, ini, sec, &(ds->link_prior[1]), "MVNORM");	// the beta's

		if (ds->link_order > 0 && (int) ds->link_prior[1].parameters[0] != ds->link_order) {
			char *ptmp;
			GMRFLib_sprintf(&ptmp, "Dimension of the MVNORM prior is not equal to the order of the link-model: %1d != %1d\n",
					(int) ds->link_prior[1].parameters[0], ds->link_order);
			inla_error_general(ptmp);
			exit(EXIT_FAILURE);
		}
		if (ds->link_order > ds->link_covariates->ncol) {
			char *ptmp;
			GMRFLib_sprintf(&ptmp, "The link-model %s require more covariates : %1d > %1d\n",
					ds->link_model, ds->link_order, ds->link_covariates->ncol);
			inla_error_general(ptmp);
			exit(EXIT_FAILURE);
		}
		ds->link_ntheta = ds->link_order + 1;
		assert(ds->link_ntheta <= LINK_MAXTHETA + 1);
		if (mb->verbose) {
			printf("\t\tlink ntheta = [%1d]\n", ds->link_ntheta);
		}

		/*
		 * mark all possible as read 
		 */
		for (i = 0; i < LINK_MAXTHETA + 1; i++) {
			char *ctmp;

			GMRFLib_sprintf(&ctmp, "LINK.FIXED%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "LINK.INITIAL%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "LINK.PRIOR%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "LINK.PARAMETERS%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "LINK.to.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "LINK.from.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);
		}

		ds->link_fixed = Calloc(ds->link_ntheta, int);
		ds->link_initial = Calloc(ds->link_ntheta, double);
		ds->link_parameters = Calloc(1, Link_param_tp);
		ds->link_parameters->order = ds->link_order;
		ds->predictor_invlinkfunc_arg = (void *) (ds->link_parameters);

		HYPER_NEW(ds->link_parameters->log_prec, 0.0);
		ds->link_parameters->betas = Calloc(LINK_MAXTHETA, double **);
		for (i = 0; i < LINK_MAXTHETA; i++) {
			HYPER_NEW(ds->link_parameters->betas[i], 0.0);
		}

		/*
		 * then read those we need 
		 */
		for (i = 0; i < ds->link_ntheta; i++) {
			double theta_initial = 0;
			char *ctmp;

			GMRFLib_sprintf(&ctmp, "LINK.FIXED%1d", i);
			ds->link_fixed[i] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);
			GMRFLib_sprintf(&ctmp, "LINK.INITIAL%1d", i);
			theta_initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), theta_initial);

			if (!ds->link_fixed[i] && mb->reuse_mode) {
				theta_initial = mb->theta_file[mb->theta_counter_file++];
			}

			if (mb->verbose) {
				printf("\t\tlink theta fixed  [%1d] = [%1d]\n", i, ds->link_fixed[i]);
				printf("\t\tlink theta initial[%1d] = [%g]\n", i, ds->link_initial[i]);
			}

			if (i == 0) {
				/*
				 * precision 
				 */
				HYPER_INIT(ds->link_parameters->log_prec, theta_initial);
				if (mb->verbose) {
					printf("\t\tlink initialise log_prec = [%g]\n", theta_initial);
					printf("\t\tlink fixed[%1d]=[%1d]\n", i, ds->link_fixed[i]);
				}

				if (!ds->link_fixed[i]) {
					/*
					 * add this \theta 
					 */
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
					GMRFLib_sprintf(&msg, "Link log precision for %s", ds->data_likelihood);
					mb->theta_tag[mb->ntheta] = inla_make_tag(msg, mb->ds);
					GMRFLib_sprintf(&msg, "Link precision for %s", ds->data_likelihood);
					mb->theta_tag_userscale[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "%s-parameter%1d", msg, i);
					mb->theta_dir[mb->ntheta] = inla_make_tag(msg, mb->ds);

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[0].to_theta);
					mb->theta[mb->ntheta] = ds->link_parameters->log_prec;
					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map[mb->ntheta] = map_precision;
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
					mb->theta_map_arg[mb->ntheta] = NULL;
					mb->ntheta++;
				}
			} else {
				/*
				 * beta
				 */
				HYPER_INIT(ds->link_parameters->betas[i - 1], theta_initial);
				if (mb->verbose) {
					printf("\t\tlink initialise beta[%1d]=[%g]\n", i, theta_initial);
					printf("\t\tlink fixed[%1d]=[%1d]\n", i, ds->link_fixed[i]);
				}

				if (!ds->link_fixed[i]) {
					/*
					 * add this \theta 
					 */
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
					GMRFLib_sprintf(&msg, "Link beta%1d for %s", i, ds->data_likelihood);
					mb->theta_tag[mb->ntheta] = inla_make_tag(msg, mb->ds);
					GMRFLib_sprintf(&msg, "Link beta%1d for %s", i, ds->data_likelihood);
					mb->theta_tag_userscale[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "%s-parameter%1d", msg, i);
					mb->theta_dir[mb->ntheta] = inla_make_tag(msg, mb->ds);

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->link_prior[1].from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->link_prior[1].to_theta);
					mb->theta[mb->ntheta] = ds->link_parameters->betas[i - 1];	/* yes! */
					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map[mb->ntheta] = map_identity;
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
					mb->theta_map_arg[mb->ntheta] = NULL;
					mb->ntheta++;
				}
			}
		}
		break;

	default:
		assert(0 == 1);
		break;
	}

	/*
	 * mixing of the likelihood 
	 */
	ds->mix_use = iniparser_getboolean(ini, inla_string_join(secname, "MIX.USE"), 0);
	if (mb->verbose) {
		printf("\t\tmix.use[%1d]\n", ds->mix_use);
	}
	if (ds->mix_use) {
		/*
		 * read mix-parameters
		 */

		char *model = NULL;

		ds->mix_nq = iniparser_getint(ini, inla_string_join(secname, "MIX.NQ"), 15);
		assert(ds->mix_nq >= 5);
		model = GMRFLib_strdup(strupc(iniparser_getstring(ini, inla_string_join(secname, "MIX.MODEL"), NULL)));
		if (mb->verbose) {
			printf("\t\tmix.nq [%d]\n", ds->mix_nq);
			printf("\t\tmix.model [%s]\n", model);
		}
		if (!strcasecmp(model, "GAUSSIAN")) {
			ds->mix_id = MIX_GAUSSIAN;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "MIX.MODEL", model);
		}

		switch (ds->mix_id) {
		case MIX_GAUSSIAN:
			/*
			 * get options related to the gaussian 
			 */
			tmp = iniparser_getdouble(ini, inla_string_join(secname, "MIX.INITIAL"), G.log_prec_initial);
			ds->mix_fixed = iniparser_getboolean(ini, inla_string_join(secname, "MIX.FIXED"), 0);
			if (!ds->mix_fixed && mb->reuse_mode) {
				tmp = mb->theta_file[mb->theta_counter_file++];
			}
			HYPER_NEW(ds->data_observations.mix_log_prec_gaussian, tmp);
			if (mb->verbose) {
				printf("\t\tinitialise mix.log_precision[%g]\n", ds->data_observations.mix_log_prec_gaussian[0][0]);
				printf("\t\tmix.fixed=[%1d]\n", ds->mix_fixed);
			}
			inla_read_prior_mix(mb, ini, sec, &(ds->mix_prior), "LOGGAMMA");

			/*
			 * add theta 
			 */
			if (!ds->mix_fixed) {
				mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
				mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
				mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
				mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
				mb->theta_tag[mb->ntheta] = inla_make_tag("Log precision for the Gaussian mix", mb->ds);
				mb->theta_tag_userscale[mb->ntheta] = inla_make_tag("Precision for the Gaussian mix", mb->ds);
				GMRFLib_sprintf(&msg, "%s-parameter", secname);
				mb->theta_dir[mb->ntheta] = msg;

				mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
				mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
				mb->theta_from[mb->ntheta] = GMRFLib_strdup(ds->mix_prior.from_theta);
				mb->theta_to[mb->ntheta] = GMRFLib_strdup(ds->mix_prior.to_theta);

				mb->theta[mb->ntheta] = ds->data_observations.mix_log_prec_gaussian;
				mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
				mb->theta_map[mb->ntheta] = map_precision;
				mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
				mb->theta_map_arg[mb->ntheta] = NULL;
				mb->ntheta++;
				ds->mix_ntheta++;
			}

			ds->mix_loglikelihood = ds->loglikelihood;
			ds->loglikelihood = loglikelihood_mix_gaussian;
			break;

		default:
			assert(0 == 1);
			break;
		}
	}

	if (ds->data_id != L_GAUSSIAN || ds->predictor_invlinkfunc != link_identity || ds->mix_use) {
		mb->gaussian_data = GMRFLib_FALSE;
	}

	return INLA_OK;
}
GMRFLib_constr_tp *inla_read_constraint(const char *filename, int n)
{
	/*
	 * read constraints from file 
	 */
	double *x = NULL;
	int i, j, m, nc;

	inla_read_data_all(&x, &m, filename);
	nc = m / (n + 1);				       /* yes, integer division */
	if (nc * n + nc != m) {
		char *msg = NULL;

		GMRFLib_sprintf(&msg, "Number of elements[%1d] in file[%s] does is not a multiplum of n=[%1d]", m, filename, n + 1);
		inla_error_general(msg);
	}
	GMRFLib_constr_tp *c = NULL;

	GMRFLib_make_empty_constr(&c);
	c->nc = nc;
	c->a_matrix = Calloc(nc * n, double);
	c->e_vector = Calloc(nc, double);

	for (j = 0; j < nc; j++) {
		for (i = 0; i < n; i++) {
			c->a_matrix[i * nc + j] = x[i + j * n];
		}
		c->e_vector[j] = x[nc * n + j];
	}
	/*
	 * this is a stupid construction (i blame myself...): i have to pass the graph in order to pass `n'... 
	 */
	GMRFLib_graph_tp *g = NULL;

	GMRFLib_make_linear_graph(&g, n, 0, 0);
	GMRFLib_prepare_constr(c, g, 0);
	GMRFLib_free_graph(g);
	Free(x);
	return c;
}
GMRFLib_constr_tp *inla_make_constraint(int n, int sumzero, GMRFLib_constr_tp * constr)
{
	/*
	 * merge the constraints, if any. yes, i have to do this manually. 
	 */

	int i, j, nc;
	GMRFLib_constr_tp *c = NULL;

	if (!sumzero && !constr) {
		return NULL;
	}

	GMRFLib_make_empty_constr(&c);
	if (sumzero && !constr) {
		/*
		 * just the sumzero constraint 
		 */
		nc = 1;
		c->nc = nc;
		c->a_matrix = Calloc(nc * n, double);
		c->e_vector = Calloc(nc, double);
		for (i = 0; i < n; i++) {
			c->a_matrix[i] = 1.0;
		}
	} else if (!sumzero && constr) {
		nc = constr->nc;
		c->nc = nc;
		c->a_matrix = Calloc(nc * n, double);
		c->e_vector = Calloc(nc, double);
		memcpy(c->a_matrix, constr->a_matrix, n * nc * sizeof(double));
		memcpy(c->e_vector, constr->e_vector, nc * sizeof(double));
	} else {
		assert(sumzero && constr);

		nc = constr->nc + 1;
		c->nc = nc;
		c->a_matrix = Calloc(nc * n, double);
		c->e_vector = Calloc(nc, double);

		for (j = 0; j < nc - 1; j++) {
			for (i = 0; i < n; i++) {
				c->a_matrix[i * nc + j] = constr->a_matrix[i * constr->nc + j];
			}
			c->e_vector[j] = constr->e_vector[j];
		}
		j = nc - 1;
		for (i = 0; i < n; i++) {
			c->a_matrix[i * nc + j] = 1.0;
		}
		c->e_vector[j] = 0.0;
	}


	/*
	 * this is a stupid construction (i blame myself...): i have to pass the graph in order to pass `n'... 
	 */
	GMRFLib_graph_tp *g = NULL;

	GMRFLib_make_linear_graph(&g, n, 0, 0);
	GMRFLib_prepare_constr(c, g, 0);
	GMRFLib_free_graph(g);

	return c;
}
GMRFLib_constr_tp *inla_make_constraint2(int n, int replicate, int sumzero, GMRFLib_constr_tp * constr)
{
	/*
	 * merge the constraints, if any. yes, i have to do this manually.
	 * this is the second version for which n is the basic size which has to be replicated.
	 */

	int i, ii, j, k, ccount, Ntotal;
	GMRFLib_constr_tp *c = NULL;

	if (!sumzero && !constr) {
		return NULL;
	}

	Ntotal = n * replicate;
	GMRFLib_make_empty_constr(&c);
	c->nc = ((constr ? constr->nc : 0) + (sumzero ? 1 : 0)) * replicate;
	c->a_matrix = Calloc(c->nc * Ntotal, double);
	c->e_vector = Calloc(c->nc, double);

	ccount = 0;
	if (sumzero) {
		for (j = 0; j < replicate; j++) {
			for (i = 0; i < n; i++) {
				ii = i + j * n;
				c->a_matrix[ii * c->nc + ccount] = 1.0;
			}
			ccount++;
		}
	}
	if (constr) {
		for (k = 0; k < constr->nc; k++) {
			for (j = 0; j < replicate; j++) {
				for (i = 0; i < n; i++) {
					ii = i + j * n;
					c->a_matrix[ii * c->nc + ccount] = constr->a_matrix[i * constr->nc + k];
				}
				c->e_vector[ccount] = constr->e_vector[k];
				ccount++;
			}
		}
	}
	assert(ccount == c->nc);

	/*
	 * this is a stupid construction (i blame myself...): i have to pass the graph in order to pass `n'... 
	 */
	GMRFLib_graph_tp *g = NULL;

	GMRFLib_make_linear_graph(&g, Ntotal, 0, 0);
	GMRFLib_prepare_constr(c, g, 0);
	GMRFLib_free_graph(g);

	return c;
}
int inla_parse_ffield(inla_tp * mb, dictionary * ini, int sec)
{
#define SET(a_, b_) mb->f_ ## a_[mb->nf] = b_
#define OneOf(a_) (!strcasecmp(model, a_))
#define OneOf2(a_, b_) (OneOf(a_) || OneOf(b_))
#define OneOf3(a_, b_, c_) (OneOf(a_) || OneOf(b_) || OneOf(c_))
#define SetInitial(id_, val_) mb->f_initial[mb->nf][id_] = val_
	/*
	 * parse section = ffield 
	 */
	int i, j, k, jj, nlocations, nc, n = 0, zn = 0, zm = 0, s = 0, itmp, id, bvalue = 0, fixed, order, slm_n = -1, slm_m = -1;
	int R2c = -1, c2R = -1, Id = -1;
	char *filename = NULL, *filenamec = NULL, *secname = NULL, *model = NULL, *ptmp = NULL, *ptmp2 = NULL, *msg = NULL, default_tag[100], *file_loc,
	    *filename_R2c = NULL, *filename_c2R = NULL, *ctmp = NULL;
	double **log_prec = NULL, **log_prec0 = NULL, **log_prec1 = NULL, **log_prec2, **phi_intern = NULL, **rho_intern = NULL, **group_rho_intern = NULL,
	    **group_prec_intern = NULL, **rho_intern01 = NULL, **rho_intern02 = NULL, **rho_intern12 = NULL, **range_intern = NULL, tmp,
	    **beta_intern = NULL, **beta = NULL, **h2_intern = NULL, **a_intern = NULL, ***theta_iidwishart = NULL, **log_diag, rd,
	    **mean_x = NULL, **log_prec_x = NULL, ***pacf_intern = NULL, slm_rho_min = 0.0, slm_rho_max = 0.0, **log_halflife = NULL, **log_shape = NULL;

	GMRFLib_crwdef_tp *crwdef = NULL;
	inla_spde_tp *spde_model = NULL;
	inla_spde_tp *spde_model_orig = NULL;
	inla_spde2_tp *spde2_model = NULL;
	inla_spde2_tp *spde2_model_orig = NULL;
	inla_spde3_tp *spde3_model = NULL;
	inla_spde3_tp *spde3_model_orig = NULL;

	if (mb->verbose) {
		printf("\tinla_parse_ffield...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\tsection=[%s]\n", secname);
	}
	mb->f_tag = Realloc(mb->f_tag, mb->nf + 1, char *);
	mb->f_dir = Realloc(mb->f_dir, mb->nf + 1, char *);
	mb->f_modelname = Realloc(mb->f_modelname, mb->nf + 1, char *);
	mb->f_c = Realloc(mb->f_c, mb->nf + 1, int *);
	mb->f_n = Realloc(mb->f_n, mb->nf + 1, int);
	mb->f_N = Realloc(mb->f_N, mb->nf + 1, int);
	mb->f_Ntotal = Realloc(mb->f_Ntotal, mb->nf + 1, int);
	mb->f_order = Realloc(mb->f_order, mb->nf + 1, int);
	mb->f_nrep = Realloc(mb->f_nrep, mb->nf + 1, int);
	mb->f_ngroup = Realloc(mb->f_ngroup, mb->nf + 1, int);
	mb->f_group_model = Realloc(mb->f_group_model, mb->nf + 1, int);
	mb->f_group_cyclic = Realloc(mb->f_group_cyclic, mb->nf + 1, int);
	mb->f_group_order = Realloc(mb->f_group_order, mb->nf + 1, int);
	mb->f_group_graph = Realloc(mb->f_group_graph, mb->nf + 1, GMRFLib_graph_tp *);
	mb->f_nrow = Realloc(mb->f_nrow, mb->nf + 1, int);
	mb->f_ncol = Realloc(mb->f_ncol, mb->nf + 1, int);
	mb->f_locations = Realloc(mb->f_locations, mb->nf + 1, double *);
	mb->f_weights = Realloc(mb->f_weights, mb->nf + 1, double *);
	mb->f_scale = Realloc(mb->f_scale, mb->nf + 1, double *);
	mb->f_Qfunc = Realloc(mb->f_Qfunc, mb->nf + 1, GMRFLib_Qfunc_tp *);
	mb->f_Qfunc_orig = Realloc(mb->f_Qfunc_orig, mb->nf + 1, GMRFLib_Qfunc_tp *);
	mb->f_Qfunc_arg = Realloc(mb->f_Qfunc_arg, mb->nf + 1, void *);
	mb->f_Qfunc_arg_orig = Realloc(mb->f_Qfunc_arg_orig, mb->nf + 1, void *);
	mb->f_bfunc2 = Realloc(mb->f_bfunc2, mb->nf + 1, GMRFLib_bfunc2_tp *);
	mb->f_graph = Realloc(mb->f_graph, mb->nf + 1, GMRFLib_graph_tp *);
	mb->f_graph_orig = Realloc(mb->f_graph_orig, mb->nf + 1, GMRFLib_graph_tp *);
	mb->f_prior = Realloc(mb->f_prior, mb->nf + 1, Prior_tp *);
	mb->f_sumzero = Realloc(mb->f_sumzero, mb->nf + 1, char);
	mb->f_constr = Realloc(mb->f_constr, mb->nf + 1, GMRFLib_constr_tp *);
	mb->f_constr_orig = Realloc(mb->f_constr_orig, mb->nf + 1, GMRFLib_constr_tp *);
	mb->f_diag = Realloc(mb->f_diag, mb->nf + 1, double);
	mb->f_compute = Realloc(mb->f_compute, mb->nf + 1, int);
	mb->f_fixed = Realloc(mb->f_fixed, mb->nf + 1, int *);
	mb->f_initial = Realloc(mb->f_initial, mb->nf + 1, double *);
	mb->f_rankdef = Realloc(mb->f_rankdef, mb->nf + 1, double);
	mb->f_id = Realloc(mb->f_id, mb->nf + 1, inla_component_tp);
	mb->f_ntheta = Realloc(mb->f_ntheta, mb->nf + 1, int);
	mb->f_cyclic = Realloc(mb->f_cyclic, mb->nf + 1, int);
	mb->f_nu = Realloc(mb->f_nu, mb->nf + 1, int);
	mb->f_Torder = Realloc(mb->f_Torder, mb->nf + 1, int);
	mb->f_Tmodel = Realloc(mb->f_Tmodel, mb->nf + 1, char *);
	mb->f_Korder = Realloc(mb->f_Korder, mb->nf + 1, int);
	mb->f_Kmodel = Realloc(mb->f_Kmodel, mb->nf + 1, char *);
	mb->f_model = Realloc(mb->f_model, mb->nf + 1, void *);
	mb->f_theta = Realloc(mb->f_theta, mb->nf + 1, double ***);
	mb->f_theta_map = Realloc(mb->f_theta_map, mb->nf + 1, map_func_tp **);
	mb->f_theta_map_arg = Realloc(mb->f_theta_map_arg, mb->nf + 1, void **);
	mb->f_of = Realloc(mb->f_of, mb->nf + 1, char *);
	mb->f_same_as = Realloc(mb->f_same_as, mb->nf + 1, char *);
	mb->f_precision = Realloc(mb->f_precision, mb->nf + 1, double);
	mb->f_output = Realloc(mb->f_output, mb->nf + 1, Output_tp *);
	mb->f_id_names = Realloc(mb->f_id_names, mb->nf + 1, inla_file_contents_tp *);
	mb->f_correct = Realloc(mb->f_correct, mb->nf + 1, int);

	/*
	 * set everything to `ZERO' initially 
	 */
	SET(c, NULL);
	SET(n, 0);
	SET(N, 0);
	SET(Ntotal, 0);
	SET(order, -1);
	SET(nrow, 0);
	SET(ncol, 0);
	SET(locations, NULL);
	SET(weights, NULL);				       /* this means default weights = 1 */
	SET(scale, NULL);
	SET(Qfunc, (GMRFLib_Qfunc_tp *) NULL);
	SET(Qfunc_orig, (GMRFLib_Qfunc_tp *) NULL);
	SET(Qfunc_arg, NULL);
	SET(Qfunc_arg_orig, NULL);
	SET(bfunc2, NULL);
	SET(graph, NULL);
	SET(graph_orig, NULL);
	SET(prior, NULL);
	SET(sumzero, 0);
	SET(constr, NULL);
	SET(constr_orig, NULL);
	SET(diag, 0.0);
	SET(compute, 1);
	SET(fixed, NULL);
	SET(initial, NULL);
	SET(rankdef, 0);
	SET(output, NULL);
	SET(id, INVALID_COMPONENT);
	SET(ntheta, 0);
	SET(theta, NULL);
	SET(cyclic, 0);
	SET(nu, -1);
	SET(Tmodel, NULL);
	SET(Torder, -1);
	SET(Kmodel, NULL);
	SET(Korder, -1);
	SET(of, NULL);
	SET(precision, 1.0e9);
	SET(nrep, 1);
	SET(ngroup, 1);
	SET(group_model, G_EXCHANGEABLE);
	SET(group_cyclic, 0);
	SET(group_order, 0);
	SET(group_graph, NULL);
	SET(id_names, NULL);
	SET(correct, -1);

	sprintf(default_tag, "default tag for ffield %d", mb->nf);
	mb->f_tag[mb->nf] = GMRFLib_strdup((secname ? secname : default_tag));
	mb->f_dir[mb->nf] = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "DIR"), inla_fnmfix(GMRFLib_strdup(mb->f_tag[mb->nf]))));
	if (mb->verbose) {
		printf("\t\tdir=[%s]\n", mb->f_dir[mb->nf]);
	}
	/*
	 * Somewhere, I might need to know this upfront... 
	 */
	HYPER_NEW(log_prec, 0.0);
	HYPER_NEW(log_prec0, 0.0);
	HYPER_NEW(log_prec1, 0.0);
	HYPER_NEW(log_prec2, 0.0);
	HYPER_NEW(phi_intern, 0.0);
	HYPER_NEW(rho_intern, 0.0);
	HYPER_NEW(rho_intern01, 0.0);
	HYPER_NEW(rho_intern02, 0.0);
	HYPER_NEW(rho_intern12, 0.0);
	HYPER_NEW(range_intern, 0.0);
	HYPER_NEW(beta_intern, 0.0);
	HYPER_NEW(beta, 1.0);
	HYPER_NEW(group_rho_intern, 0.0);
	HYPER_NEW(group_prec_intern, 0.0);
	HYPER_NEW(h2_intern, 0.0);
	HYPER_NEW(a_intern, 0.0);
	HYPER_NEW(log_diag, 0.0);
	HYPER_NEW(mean_x, 0.0);
	HYPER_NEW(log_prec_x, 0.0);
	HYPER_NEW(log_halflife, 0.0);
	HYPER_NEW(log_shape, 0.0);

	/*
	 * start parsing 
	 */
	model = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "MODEL"), NULL));
	if (mb->verbose) {
		printf("\t\tmodel=[%s]\n", model);
	}

	if (OneOf("RW2D")) {
		mb->f_id[mb->nf] = F_RW2D;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Random walk 2D");
	} else if (OneOf("RW2DIID")) {
		mb->f_id[mb->nf] = F_RW2DIID;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Random walk 2DIID");
	} else if (OneOf("BESAG")) {
		mb->f_id[mb->nf] = F_BESAG;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Besags ICAR model");
	} else if (OneOf("BESAG2")) {
		mb->f_id[mb->nf] = F_BESAG2;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Besags ICAR model for joint models");
	} else if (OneOf("BYM")) {
		mb->f_id[mb->nf] = F_BYM;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("BYM model");
	} else if (OneOf("BYM2")) {
		mb->f_id[mb->nf] = F_BYM2;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("BYM2 model");
	} else if (OneOf("BESAGPROPER")) {
		mb->f_id[mb->nf] = F_BESAGPROPER;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Proper version of Besags ICAR model");
	} else if (OneOf("BESAGPROPER2")) {
		mb->f_id[mb->nf] = F_BESAGPROPER2;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Proper version of Besags ICAR model version 2");
	} else if (OneOf2("GENERIC", "GENERIC0")) {
		mb->f_id[mb->nf] = F_GENERIC0;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Generic0 model");
	} else if (OneOf("GENERIC1")) {
		mb->f_id[mb->nf] = F_GENERIC1;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Generic1 model");
	} else if (OneOf("GENERIC2")) {
		mb->f_id[mb->nf] = F_GENERIC2;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Generic2 model");
	} else if (OneOf("GENERIC3")) {
		mb->f_id[mb->nf] = F_GENERIC3;
		mb->f_ntheta[mb->nf] = GENERIC3_MAXTHETA;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Generic3 model");
	} else if (OneOf("SEASONAL")) {
		mb->f_id[mb->nf] = F_SEASONAL;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Seasonal model");
	} else if (OneOf("IID")) {
		mb->f_id[mb->nf] = F_IID;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID model");
	} else if (OneOf("IID1D")) {
		mb->f_id[mb->nf] = F_IID1D;
		mb->f_ntheta[mb->nf] = inla_iid_wishart_nparam(WISHART_DIM(mb->nf));
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID1D model");
	} else if (OneOf("IID2D")) {
		mb->f_id[mb->nf] = F_IID2D;
		mb->f_ntheta[mb->nf] = inla_iid_wishart_nparam(WISHART_DIM(mb->nf));
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID2D model");
	} else if (OneOf("IID3D")) {
		mb->f_id[mb->nf] = F_IID3D;
		mb->f_ntheta[mb->nf] = inla_iid_wishart_nparam(WISHART_DIM(mb->nf));
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID3D model");
	} else if (OneOf("IID4D")) {
		mb->f_id[mb->nf] = F_IID4D;
		mb->f_ntheta[mb->nf] = inla_iid_wishart_nparam(WISHART_DIM(mb->nf));
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID4D model");
	} else if (OneOf("IID5D")) {
		mb->f_id[mb->nf] = F_IID5D;
		mb->f_ntheta[mb->nf] = inla_iid_wishart_nparam(WISHART_DIM(mb->nf));
		mb->f_modelname[mb->nf] = GMRFLib_strdup("IID5D model");
	} else if (OneOf("2DIID")) {
		mb->f_id[mb->nf] = F_2DIID;
		mb->f_ntheta[mb->nf] = 3;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("2DIID model");
	} else if (OneOf("MEC")) {
		mb->f_id[mb->nf] = F_MEC;
		mb->f_ntheta[mb->nf] = 4;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("MEC");
	} else if (OneOf("MEB")) {
		mb->f_id[mb->nf] = F_MEB;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("MEB");
	} else if (OneOf("RGENERIC")) {
		mb->f_id[mb->nf] = F_R_GENERIC;
		mb->f_ntheta[mb->nf] = -1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("RGeneric");
	} else if (OneOf("RW1")) {
		mb->f_id[mb->nf] = F_RW1;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("RW1 model");
	} else if (OneOf("RW2")) {
		mb->f_id[mb->nf] = F_RW2;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("RW2 model");
	} else if (OneOf("CRW2")) {
		mb->f_id[mb->nf] = F_CRW2;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("CRW2 model");
	} else if (OneOf("AR1")) {
		mb->f_id[mb->nf] = F_AR1;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("AR1 model");
	} else if (OneOf("OU")) {
		mb->f_id[mb->nf] = F_OU;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Ornstein-Uhlenbeck model");
	} else if (OneOf("MATERN2D")) {
		mb->f_id[mb->nf] = F_MATERN2D;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Matern2D model");
	} else if (OneOf("Z")) {
		mb->f_id[mb->nf] = F_Z;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Z model");
	} else if (OneOf("SLM")) {
		mb->f_id[mb->nf] = F_SLM;
		mb->f_ntheta[mb->nf] = 2;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("SLM model");
	} else if (OneOf("SPDE")) {
		mb->f_id[mb->nf] = F_SPDE;
		mb->f_ntheta[mb->nf] = 4;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("SPDE model");
	} else if (OneOf("SPDE2")) {
		mb->f_id[mb->nf] = F_SPDE2;
		mb->f_ntheta[mb->nf] = -1;		       /* Not known yet */
		mb->f_modelname[mb->nf] = GMRFLib_strdup("SPDE2 model");
	} else if (OneOf("SPDE3")) {
		mb->f_id[mb->nf] = F_SPDE3;
		mb->f_ntheta[mb->nf] = -1;		       /* Not known yet */
		mb->f_modelname[mb->nf] = GMRFLib_strdup("SPDE3 model");
	} else if (OneOf("AR")) {
		mb->f_id[mb->nf] = F_AR;
		mb->f_ntheta[mb->nf] = -1;		       /* Not known yet */
		mb->f_modelname[mb->nf] = GMRFLib_strdup("AR(p) model");
	} else if (OneOf("COPY")) {
		mb->f_id[mb->nf] = F_COPY;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Copy");
	} else if (OneOf("CLINEAR")) {
		mb->f_id[mb->nf] = F_CLINEAR;
		mb->f_ntheta[mb->nf] = 1;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Constrained linear");
	} else if (OneOf("SIGM")) {
		mb->f_id[mb->nf] = F_SIGM;
		mb->f_ntheta[mb->nf] = 3;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Sigmodial");
	} else if (OneOf("REVSIGM")) {
		mb->f_id[mb->nf] = F_REVSIGM;
		mb->f_ntheta[mb->nf] = 3;
		mb->f_modelname[mb->nf] = GMRFLib_strdup("Reverse sigmodial");
	} else {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, "model", model);
	}
	if (mb->f_ntheta[mb->nf] > 0) {
		mb->f_prior[mb->nf] = Calloc(mb->f_ntheta[mb->nf], Prior_tp);
		mb->f_fixed[mb->nf] = Calloc(mb->f_ntheta[mb->nf], int);
		mb->f_theta[mb->nf] = Calloc(mb->f_ntheta[mb->nf], double **);
	} else {
		mb->f_prior[mb->nf] = NULL;
		mb->f_fixed[mb->nf] = NULL;
		mb->f_theta[mb->nf] = NULL;
	}


	/*
	 * just allocate this here, as its needed all over 
	 */
	if (mb->f_ntheta[mb->nf] > 0) {
		mb->f_initial[mb->nf] = Calloc(mb->f_ntheta[mb->nf], double);
	}

	id = mb->f_id[mb->nf];				       /* shortcut */

	switch (id) {
	case F_RW2D:
	case F_BESAG:
	case F_GENERIC0:
	case F_SEASONAL:
	case F_IID:
	case F_RW1:
	case F_RW2:
	case F_CRW2:
	case F_Z:
		inla_read_prior(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");
		break;

	case F_SLM:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	// kappa
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "NORMAL");	// rho
		break;

	case F_BESAG2:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	// kappa
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "NORMAL-a");	// a
		break;

	case F_BESAGPROPER:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	// precision
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	// weight
		break;

	case F_BESAGPROPER2:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	// precision
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN");	// lambda
		break;

	case F_SPDE:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL");	// T[0]
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "NORMAL");	// K[0]
		inla_read_prior2(mb, ini, sec, &(mb->f_prior[mb->nf][2]), "NORMAL");	// the rest
		inla_read_prior3(mb, ini, sec, &(mb->f_prior[mb->nf][3]), "FLAT");	// the ocillating cooef
		break;

	case F_SPDE2:
		mb->f_prior[mb->nf] = Calloc(1, Prior_tp);
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "MVNORM");	// Just one prior...
		break;

	case F_SPDE3:
		mb->f_prior[mb->nf] = Calloc(1, Prior_tp);
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "MVNORM");	// Just one prior...
		break;

	case F_AR:
		mb->f_prior[mb->nf] = Calloc(2, Prior_tp);
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "PCPREC");	// log precision
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "PCAR");	// the pacf
		break;

	case F_COPY:
		inla_read_prior(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL-1");
		break;

	case F_CLINEAR:
		inla_read_prior(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL");
		break;

	case F_SIGM:
	case F_REVSIGM:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL");	// beta
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "NORMAL");	// log_halflife
		inla_read_prior2(mb, ini, sec, &(mb->f_prior[mb->nf][2]), "NORMAL");	// log_shape
		break;

	case F_IID1D:
	case F_IID2D:
	case F_IID3D:
	case F_IID4D:
	case F_IID5D:
	{
		int dim = WISHART_DIM(mb->nf);
		assert(dim > 0);
		char *pri, *par, *to_theta, *from_theta, *prifunc;
		int nt = inla_iid_wishart_nparam(dim);

		GMRFLib_sprintf(&prifunc, "WISHART%1dD", dim);
		int kk;
		if (dim > 1) {
			for (kk = 0; kk < nt; kk++) {
				GMRFLib_sprintf(&pri, "PRIOR%1d", kk);
				GMRFLib_sprintf(&par, "PARAMETERS%1d", kk);
				GMRFLib_sprintf(&from_theta, "FROM.THETA%1d", kk);
				GMRFLib_sprintf(&to_theta, "TO.THETA%1d", kk);
				inla_read_prior_generic(mb, ini, sec, &(mb->f_prior[mb->nf][kk]), pri, par, from_theta, to_theta, (kk == 0 ? prifunc : "NONE"));

				Free(pri);
				Free(par);
				Free(from_theta);
				Free(to_theta);
			}
		} else {
			kk = 0;
			GMRFLib_sprintf(&pri, "PRIOR");
			GMRFLib_sprintf(&par, "PARAMETERS");
			GMRFLib_sprintf(&from_theta, "FROM.THETA");
			GMRFLib_sprintf(&to_theta, "TO.THETA");
			inla_read_prior_generic(mb, ini, sec, &(mb->f_prior[mb->nf][kk]), pri, par, from_theta, to_theta, (kk == 0 ? prifunc : "NONE"));
		}

		Free(pri);
		Free(par);
	}
		break;

	case F_BYM:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision0 iid */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* precision1 spatial */
		break;

	case F_BYM2:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN");	/* phi */
		break;

	case F_RW2DIID:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN");	/* phi */
		break;

	case F_AR1:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* marginal precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN-rho");	/* phi (lag-1 correlation) */
		break;

	case F_OU:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* marginal precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN");	/* log(phi) */
		break;

	case F_GENERIC1:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "GAUSSIAN");	/* beta */
		break;

	case F_GENERIC2:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision Cmatrix */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* the other precision, but theta1 = h^2 */
		break;

	case F_GENERIC3:
		for (i = 0; i < GENERIC3_MAXTHETA; i++) {
			inla_read_priorN(mb, ini, sec, &(mb->f_prior[mb->nf][i]), "LOGGAMMA", i);
		}
		break;

	case F_2DIID:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision0 */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* precision1 */
		inla_read_prior2(mb, ini, sec, &(mb->f_prior[mb->nf][2]), "GAUSSIAN-rho");	/* rho */
		break;

	case F_MATERN2D:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "LOGGAMMA");	/* precision */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* range */
		break;

	case F_MEC:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL");	/* beta */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* prec.u */
		inla_read_prior2(mb, ini, sec, &(mb->f_prior[mb->nf][2]), "NORMAL");	/* mean */
		inla_read_prior3(mb, ini, sec, &(mb->f_prior[mb->nf][3]), "LOGGAMMA");	/* prec.x */
		break;

	case F_MEB:
		inla_read_prior0(mb, ini, sec, &(mb->f_prior[mb->nf][0]), "NORMAL");	/* beta */
		inla_read_prior1(mb, ini, sec, &(mb->f_prior[mb->nf][1]), "LOGGAMMA");	/* prec.u */
		break;

	case F_R_GENERIC:
		break;

	default:
		abort();
	}

	mb->f_correct[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "CORRECT"), -1);
	if (mb->verbose) {
		printf("\t\tcorrect=[%1d]\n", mb->f_correct[mb->nf]);
	}
	mb->f_sumzero[mb->nf] = (char) iniparser_getboolean(ini, inla_string_join(secname, "CONSTRAINT"), 0);
	if (mb->verbose) {
		printf("\t\tconstr=[%1d]\n", mb->f_sumzero[mb->nf]);
	}
	mb->f_diag[mb->nf] = iniparser_getdouble(ini, inla_string_join(secname, "DIAGONAL"), 0.0);
	if (mb->verbose) {
		printf("\t\tdiagonal=[%g]\n", mb->f_diag[mb->nf]);
	}
	mb->f_id_names[mb->nf] = inla_read_file_contents(GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "ID.NAMES"), NULL)));
	if (mb->verbose) {
		printf("\t\tid.names=%s\n", (mb->f_id_names[mb->nf] ? "<read>" : "<not present>"));
	}

	mb->f_compute[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "COMPUTE"), 1);
	if (G.mode == INLA_MODE_HYPER) {
		if (mb->f_compute[mb->nf]) {
			fprintf(stderr, "*** Warning: HYPER_MODE require f_compute[%1d] = 0\n", mb->nf);
		}
		mb->f_compute[mb->nf] = 0;
	}
	if (mb->verbose) {
		printf("\t\tcompute=[%1d]\n", mb->f_compute[mb->nf]);
	}
	if (mb->f_ntheta[mb->nf] == 1) {
		mb->f_fixed[mb->nf][0] = iniparser_getboolean(ini, inla_string_join(secname, "FIXED"), 0);
	} else {
		/*
		 * reads FIXED0, FIXED1, FIXED2, etc.... 
		 */
		for (i = 0; i < mb->f_ntheta[mb->nf]; i++) {
			char *fixname;

			GMRFLib_sprintf(&fixname, "FIXED%1d", i);
			mb->f_fixed[mb->nf][i] = iniparser_getboolean(ini, inla_string_join(secname, fixname), 0);
			Free(fixname);
		}
	}

	mb->f_nrep[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "NREP"), mb->f_nrep[mb->nf]);
	mb->f_ngroup[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "NGROUP"), mb->f_ngroup[mb->nf]);
	if (mb->verbose) {
		printf("\t\tnrep=[%1d]\n", mb->f_nrep[mb->nf]);
		printf("\t\tngroup=[%1d]\n", mb->f_ngroup[mb->nf]);
	}

	/*
	 * to avoid errors from the R-interface. This just mark them as 'read'. For some reason file_loc is fine here, but later on it just return "", FIXME! 
	 */
	ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
	file_loc = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "LOCATIONS"), NULL));

	if ((mb->f_id[mb->nf] == F_MATERN2D) || (mb->f_id[mb->nf] == F_RW2D) || (mb->f_id[mb->nf] == F_RW2DIID)) {
		/*
		 * this case is a bit special....
		 *
		 * if predictor->nrow exists this is the default and must be equal if NROW exists. same with NCOL.
		 */
		char *tmp_nrow = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "NROW"), NULL));
		char *tmp_ncol = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "NCOL"), NULL));

		if (!tmp_nrow) {
			inla_error_missing_required_field(__GMRFLib_FuncName, secname, "nrow");
		} else {
			if (inla_sread_ints(&(mb->f_nrow[mb->nf]), 1, tmp_nrow) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "nrow", tmp_nrow);
			}
			if (mb->f_nrow[mb->nf] <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "nrow", tmp_nrow);
			}
		}
		if (!tmp_ncol) {
			inla_error_missing_required_field(__GMRFLib_FuncName, secname, "ncol");
		} else {
			if (inla_sread_ints(&(mb->f_ncol[mb->nf]), 1, tmp_ncol) == INLA_FAIL) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "ncol", tmp_ncol);
			}
			if (mb->f_ncol[mb->nf] <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "ncol", tmp_ncol);
			}
		}
		mb->f_N[mb->nf] = mb->f_n[mb->nf] = mb->f_nrow[mb->nf] * mb->f_ncol[mb->nf];
		if (mb->verbose) {
			printf("\t\tnrow[%1d] x ncol[%1d] = n[%1d] \n", mb->f_nrow[mb->nf], mb->f_ncol[mb->nf], mb->f_n[mb->nf]);
		}
		mb->f_cyclic[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "CYCLIC"), 0);
		if (mb->verbose) {
			printf("\t\tcyclic=[%1d]\n", mb->f_cyclic[mb->nf]);
		}

		bvalue = iniparser_getint(ini, inla_string_join(secname, "BVALUE"), GMRFLib_BVALUE_DEFAULT);
		if (bvalue != GMRFLib_BVALUE_DEFAULT && bvalue != GMRFLib_BVALUE_ZERO) {
			GMRFLib_sprintf(&msg, "%d", bvalue);
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "BVALUE", (const char *) msg);
		}
		if (mb->verbose) {
			printf("\t\tbvalue=[%s]\n", (bvalue == GMRFLib_BVALUE_DEFAULT ? "Default" : "Zero outside"));
		}
		/*
		 * read the covariates 
		 */
		filenamec = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "COVARIATES"), NULL));
		if (!filenamec) {
			inla_error_missing_required_field(__GMRFLib_FuncName, secname, "covariates");
		}
		if (mb->verbose) {
			printf("\t\tread covariates from file=[%s]\n", filenamec);
		}
		inla_read_data_general(NULL, &(mb->f_c[mb->nf]), &nc, filenamec, mb->predictor_n, 0, 1, mb->verbose, -1.0);

		/*
		 * read weights 
		 */
		filenamec = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "WEIGHTS"), NULL));
		if (!filenamec) {
			mb->f_weights[mb->nf] = NULL;	       /* default */
		} else {
			if (mb->verbose) {
				printf("\t\tread weights from file=[%s]\n", filenamec);
			}
			inla_read_data_general(&(mb->f_weights[mb->nf]), NULL, NULL, filenamec, mb->predictor_n, 0, 1, mb->verbose, 1.0);
		}
	} else {
		/*
		 * read the covariates 
		 */
		filenamec = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "COVARIATES"), NULL));
		if (!filenamec) {
			inla_error_missing_required_field(__GMRFLib_FuncName, secname, "covariates");
		}
		if (mb->verbose) {
			printf("\t\tread covariates from file=[%s]\n", filenamec);
		}
		inla_read_data_general(NULL, &(mb->f_c[mb->nf]), &nc, filenamec, mb->predictor_n, 0, 1, mb->verbose, -1.0);

		/*
		 * read weights 
		 */
		filenamec = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "WEIGHTS"), NULL));
		if (!filenamec) {
			mb->f_weights[mb->nf] = NULL;
		} else {
			if (mb->verbose) {
				printf("\t\tread weights from file=[%s]\n", filenamec);
			}
			inla_read_data_general(&(mb->f_weights[mb->nf]), NULL, NULL, filenamec, mb->predictor_n, 0, 1, mb->verbose, 1.0);
		}

		switch (mb->f_id[mb->nf]) {
		case F_GENERIC0:
		{
			/*
			 * use field: QMATRIX to set both graph and n.
			 */
			GMRFLib_tabulate_Qfunc_tp *tab = NULL;

			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CMATRIX"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "Cmatrix");
			}
			if (mb->verbose) {
				printf("\t\tread Cmatrix from file=[%s]\n", filename);
			}
			GMRFLib_tabulate_Qfunc_from_file(&tab, &(mb->f_graph[mb->nf]), (const char *) filename, -1, NULL, NULL, log_prec);
			mb->f_Qfunc[mb->nf] = tab->Qfunc;
			mb->f_Qfunc_arg[mb->nf] = tab->Qfunc_arg;
			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = mb->f_graph[mb->nf]->n;
			mb->f_rankdef[mb->nf] = 0.0;	       /* default */
			if (mb->verbose) {
				for (i = 0; i < IMIN(PREVIEW, mb->f_graph[mb->nf]->n); i++) {
					printf("\t\t\tQ(%1d,%1d) = %g\n", i, i, tab->Qfunc(i, i, tab->Qfunc_arg));
				}
			}
			break;
		}

		case F_GENERIC1:
		{
			/*
			 * use field: CMATRIX to set both graph and n.
			 */
			inla_generic1_tp *arg = Calloc(1, inla_generic1_tp);
			int nn = -1;
			GMRFLib_graph_tp *g = NULL;

			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CMATRIX"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "Cmatrix");
			}
			if (mb->verbose) {
				printf("\t\tread Cmatrix from file=[%s]\n", filename);
			}
			GMRFLib_tabulate_Qfunc_from_file(&(arg->tab), &(mb->f_graph[mb->nf]), (const char *) filename, -1, NULL, NULL, NULL);
			arg->log_prec = log_prec;
			arg->beta = beta_intern;
			arg->n = nn = mb->f_graph[mb->nf]->n;

			/*
			 * we need to compute the eigenvalues 
			 */
			gsl_matrix *C;
			gsl_vector *evalues;
			gsl_eigen_symm_workspace *w;

			g = mb->f_graph[mb->nf];
			C = gsl_matrix_calloc(nn, nn);
			evalues = gsl_vector_calloc(nn);
			w = gsl_eigen_symm_alloc(nn);
			for (i = 0; i < nn; i++) {
				gsl_matrix_set(C, i, i, arg->tab->Qfunc(i, i, arg->tab->Qfunc_arg));
				for (jj = 0; jj < g->nnbs[i]; jj++) {
					j = g->nbs[i][jj];
					if (j > i) {
						double val = arg->tab->Qfunc(i, j, arg->tab->Qfunc_arg);
						gsl_matrix_set(C, i, j, val);
						gsl_matrix_set(C, j, i, val);
					}
				}
			}
			gsl_eigen_symm(C, evalues, w);
			arg->eigenvalues = Calloc(nn, double);
			arg->max_eigenvalue = arg->eigenvalues[0];
			arg->min_eigenvalue = arg->eigenvalues[0];
			for (i = 0; i < nn; i++) {
				arg->eigenvalues[i] = gsl_vector_get(evalues, i);
				arg->max_eigenvalue = DMAX(arg->max_eigenvalue, arg->eigenvalues[i]);
				arg->min_eigenvalue = DMIN(arg->min_eigenvalue, arg->eigenvalues[i]);
			}
			assert(arg->max_eigenvalue > 0.0);
			gsl_eigen_symm_free(w);
			gsl_matrix_free(C);
			gsl_vector_free(evalues);

			if (mb->verbose) {
				printf("\t\tMaxmimum eigenvalue = %.12g\n", arg->max_eigenvalue);
				printf("\t\tMinimum  eigenvalue = %.12g\n", arg->min_eigenvalue);
			}

			mb->f_Qfunc[mb->nf] = Qfunc_generic1;
			mb->f_Qfunc_arg[mb->nf] = (void *) arg;
			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = nn;
			mb->f_rankdef[mb->nf] = 0.0;	       /* default */
			if (mb->verbose) {
				for (i = 0; i < IMIN(PREVIEW, mb->f_graph[mb->nf]->n); i++) {
					printf("\t\t\tC(%1d,%1d) = %g\n", i, i, arg->tab->Qfunc(i, i, arg->tab->Qfunc_arg));
				}
			}
			break;
		}

		case F_GENERIC2:
		{
			/*
			 * use field: CMATRIX to set both graph and n.
			 */
			inla_generic2_tp *arg = Calloc(1, inla_generic2_tp);
			int nn = -1, ii;
			GMRFLib_graph_tp *g = NULL;

			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CMATRIX"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "Cmatrix");
			}
			if (mb->verbose) {
				printf("\t\tread Cmatrix from file=[%s]\n", filename);
			}
			GMRFLib_tabulate_Qfunc_from_file(&(arg->tab), &g, (const char *) filename, -1, NULL, NULL, NULL);
			arg->log_prec = log_prec;
			arg->h2_intern = h2_intern;
			arg->n = nn = g->n;
			arg->N = 2 * arg->n;

			GMRFLib_ged_tp *ged;
			GMRFLib_ged_init(&ged, NULL);
			for (ii = 0; ii < nn; ii++) {
				GMRFLib_ged_add(ged, ii, ii + nn);
			}
			GMRFLib_ged_insert_graph(ged, g, nn);
			GMRFLib_free_graph(g);
			GMRFLib_ged_build(&g, ged);
			assert(g->n == 2 * nn);
			GMRFLib_ged_free(ged);

			mb->f_graph[mb->nf] = g;
			mb->f_Qfunc[mb->nf] = Qfunc_generic2;
			mb->f_Qfunc_arg[mb->nf] = (void *) arg;
			mb->f_locations[mb->nf] = NULL;
			mb->f_n[mb->nf] = nn;
			mb->f_N[mb->nf] = 2 * nn;
			mb->f_rankdef[mb->nf] = 0.0;	       /* default */
			if (mb->verbose) {
				for (i = 0; i < IMIN(PREVIEW, mb->f_graph[mb->nf]->n); i++) {
					printf("\t\t\tC(%1d,%1d) = %g\n", i, i, arg->tab->Qfunc(i, i, arg->tab->Qfunc_arg));
				}
			}
			break;
		}

		case F_GENERIC3:
		{
			inla_generic3_tp *arg = Calloc(1, inla_generic3_tp);
			inla_generic3_tp *arg_orig = Calloc(1, inla_generic3_tp);

			arg->n = iniparser_getint(ini, inla_string_join(secname, "generic3.n"), -1);
			arg_orig->n = iniparser_getint(ini, inla_string_join(secname, "generic3.n"), -1);
			arg->m = iniparser_getint(ini, inla_string_join(secname, "generic3.m"), -1);
			arg_orig->m = iniparser_getint(ini, inla_string_join(secname, "generic3.m"), -1);
			if (mb->verbose) {
				printf("\t\tread generic3.n[%1d]\n", arg->n);
				printf("\t\tread generic3.m[%1d]\n", arg->m);
			}
			assert(arg->n > 0);
			assert(arg->m > 0 && arg->m < GENERIC3_MAXTHETA);

			arg->g = Calloc(arg->m, GMRFLib_graph_tp *);
			arg_orig->g = Calloc(arg->m, GMRFLib_graph_tp *);
			arg->tab = Calloc(arg->m, GMRFLib_tabulate_Qfunc_tp *);
			arg_orig->tab = Calloc(arg->m, GMRFLib_tabulate_Qfunc_tp *);
			for (k = 0; k < arg->m; k++) {
				ctmp = NULL;
				GMRFLib_sprintf(&ctmp, "generic3.Cmatrix.%1d", k);
				filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL));
				assert(filename != NULL);
				if (mb->verbose) {
					printf("\t\tread Cmatrix[[%1d]] from file=[%s]\n", k, filename);
				}
				GMRFLib_tabulate_Qfunc_from_file(&(arg->tab[k]), &(arg->g[k]), filename, arg->n, NULL, NULL, NULL);
				GMRFLib_tabulate_Qfunc_from_file(&(arg_orig->tab[k]), &(arg_orig->g[k]), filename, arg_orig->n, NULL, NULL, NULL);
				Free(ctmp);
				Free(filename);
			}
			GMRFLib_union_graph(&(arg->graph), arg->g, arg->m);
			GMRFLib_union_graph(&(arg_orig->graph), arg_orig->g, arg_orig->m);

			mb->f_graph[mb->nf] = arg->graph;
			mb->f_graph_orig[mb->nf] = arg_orig->graph;
			mb->f_Qfunc[mb->nf] = Qfunc_generic3;
			mb->f_Qfunc_orig[mb->nf] = Qfunc_generic3;
			mb->f_Qfunc_arg[mb->nf] = (void *) arg;
			mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
			mb->f_locations[mb->nf] = NULL;
			mb->f_n[mb->nf] = arg->n;
			mb->f_N[mb->nf] = arg->n;
			mb->f_rankdef[mb->nf] = 0.0;	       /* default */
			break;
		}

		case F_BESAG:
			/*
			 * use field: GRAPH. use this to set field N 
			 */
			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GRAPH"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "graph");
			}
			if (mb->verbose) {
				printf("\t\tread graph from file=[%s]\n", filename);
			}
			GMRFLib_read_graph(&(mb->f_graph[mb->nf]), filename);
			if (mb->f_graph[mb->nf]->n <= 0) {
				GMRFLib_sprintf(&msg, "graph=[%s] has zero size", filename);
				inla_error_general(msg);
			}

			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = mb->f_graph[mb->nf]->n;
			break;

		case F_BESAG2:
			/*
			 * use field: GRAPH. use this to set field N 
			 */
			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GRAPH"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "graph");
			}
			if (mb->verbose) {
				printf("\t\tread graph from file=[%s]\n", filename);
			}
			GMRFLib_read_graph(&(mb->f_graph[mb->nf]), filename);
			if (mb->f_graph[mb->nf]->n <= 0) {
				GMRFLib_sprintf(&msg, "graph=[%s] has zero size", filename);
				inla_error_general(msg);
			}
			mb->f_precision[mb->nf] = iniparser_getdouble(ini, inla_string_join(secname, "PRECISION"), mb->f_precision[mb->nf]);
			if (mb->verbose) {
				printf("\t\tprecision=[%f]\n", mb->f_precision[mb->nf]);
			}
			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = 2 * mb->f_graph[mb->nf]->n;	/* YES */
			break;

		case F_BYM:
		case F_BYM2:
			/*
			 * use field: GRAPH. use this to set field N 
			 */
			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GRAPH"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "graph");
			}
			if (mb->verbose) {
				printf("\t\tread graph from file=[%s]\n", filename);
			}
			GMRFLib_read_graph(&(mb->f_graph[mb->nf]), filename);
			if (mb->f_graph[mb->nf]->n <= 0) {
				GMRFLib_sprintf(&msg, "graph=[%s] has zero size", filename);
				inla_error_general(msg);
			}
			mb->f_locations[mb->nf] = NULL;
			mb->f_n[mb->nf] = mb->f_graph[mb->nf]->n;
			mb->f_N[mb->nf] = 2 * mb->f_n[mb->nf]; /* yes */
			break;

		case F_BESAGPROPER:
		case F_BESAGPROPER2:
			/*
			 * use field: GRAPH. use this to set field N 
			 */
			filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GRAPH"), NULL));
			if (!filename) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "graph");
			}
			if (mb->verbose) {
				printf("\t\tread graph from file=[%s]\n", filename);
			}
			GMRFLib_read_graph(&(mb->f_graph[mb->nf]), filename);
			if (mb->f_graph[mb->nf]->n <= 0) {
				GMRFLib_sprintf(&msg, "graph=[%s] has zero size", filename);
				inla_error_general(msg);
			}

			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = mb->f_graph[mb->nf]->n;
			break;

		case F_SEASONAL:
			/*
			 * seasonal component; need length N, seasonal length SEASON, and a boolean CYCLIC
			 */
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "N");
			}
			n = iniparser_getint(ini, inla_string_join(secname, "N"), 0);
			if (n <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
			}
			if (mb->verbose) {
				printf("\t\tn=[%1d]\n", n);
			}
			Free(ptmp);
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SEASON"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "SEASON");
			}
			s = iniparser_getint(ini, inla_string_join(secname, "SEASON"), 0);
			if (s <= 0 || s > n) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "SEASON", ptmp);
			}
			if (mb->verbose) {
				printf("\t\tseason=[%1d]\n", s);
			}
			/*
			 * this special option is only valid for model=iid,rw1,rw2 and locations=default. therefore we do not add it
			 * into the mb->f_.... 
			 */
			mb->f_cyclic[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "CYCLIC"), 0);
			if (mb->verbose) {
				printf("\t\tcyclic=[%1d]\n", mb->f_cyclic[mb->nf]);
			}
			Free(ptmp);
			mb->f_locations[mb->nf] = NULL;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = n;
			break;

		case F_AR1:
			/*
			 * AR1-model; need length N and a boolean CYCLIC
			 */
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "N");
			}
			n = iniparser_getint(ini, inla_string_join(secname, "N"), 0);
			if (n <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
			}
			if (mb->verbose) {
				printf("\t\tn=[%1d]\n", n);
			}
			Free(ptmp);
			mb->f_cyclic[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "CYCLIC"), 0);
			if (mb->verbose) {
				printf("\t\tcyclic=[%1d]\n", mb->f_cyclic[mb->nf]);
			}
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = n;
			break;

		case F_AR:
			/*
			 * AR(p)-model; need length N and order P
			 */
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "N");
			}
			n = iniparser_getint(ini, inla_string_join(secname, "N"), 0);
			if (n <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
			}
			if (mb->verbose) {
				printf("\t\tn=[%1d]\n", n);
			}
			Free(ptmp);

			order = iniparser_getint(ini, inla_string_join(secname, "ORDER"), 1);
			if (mb->verbose) {
				printf("\t\torder=[%1d]\n", order);
			}
			mb->f_order[mb->nf] = order;
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = n;
			break;

		case F_Z:
			/*
			 * Z-model. Here Z is a n x m matrix, and the dimension of the model is (Z*z,z) which is n+m
			 */
			zn = iniparser_getint(ini, inla_string_join(secname, "z.n"), 0);
			zm = iniparser_getint(ini, inla_string_join(secname, "z.m"), 0);
			if (zn == 0) {
				GMRFLib_sprintf(&ctmp, "%1d", zn);
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "z.n", ctmp);
			}
			if (zm == 0) {
				GMRFLib_sprintf(&ctmp, "%1d", zm);
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "z.m", ctmp);
			}
			/*
			 * if given, then argument N must be equal n+m.
			 */
			itmp = iniparser_getint(ini, inla_string_join(secname, "N"), -1);
			if (itmp > -1 && zn + zm != itmp) {
				GMRFLib_sprintf(&ctmp, "Model z: dim(Z)[2] = %1d  !=  argument.N = %1d", zm, itmp);
				inla_error_general(ctmp);
			}
			if (mb->verbose) {
				printf("\t\tz.n=[%1d]\n", zn);
				printf("\t\tz.m=[%1d]\n", zm);
			}
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = zn + zm;	/* Yes, this is correct */
			break;

		case F_SLM:
			/*
			 * SLM-model. 
			 */
			slm_n = iniparser_getint(ini, inla_string_join(secname, "slm.n"), 0);
			slm_m = iniparser_getint(ini, inla_string_join(secname, "slm.m"), 0);
			if (slm_n <= 0) {
				GMRFLib_sprintf(&ctmp, "%1d", slm_n);
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "slm.n", ctmp);
			}
			if (slm_m < 0) {
				GMRFLib_sprintf(&ctmp, "%1d", slm_m);
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "slm.m", ctmp);
			}

			/*
			 * if given, then argument N must be equal n+m.
			 */
			itmp = iniparser_getint(ini, inla_string_join(secname, "N"), -1);
			if (itmp > -1 && slm_n + slm_m != itmp) {
				GMRFLib_sprintf(&ctmp, "Model SLM: N=%1d != slm.n=%1d + slm.m=%1d\n", itmp, slm_n, slm_m);
				inla_error_general(ctmp);
			}
			if (mb->verbose) {
				printf("\t\tslm.n=[%1d]\n", slm_n);
				printf("\t\tslm.m=[%1d]\n", slm_m);
			}

			slm_rho_min = iniparser_getdouble(ini, inla_string_join(secname, "slm.rho.min"), 0);
			slm_rho_max = iniparser_getdouble(ini, inla_string_join(secname, "slm.rho.max"), 0);
			if (mb->verbose) {
				printf("\t\tslm.rho.min=[%g]\n", slm_rho_min);
				printf("\t\tslm.rho.max=[%g]\n", slm_rho_max);
			}

			mb->f_N[mb->nf] = mb->f_n[mb->nf] = slm_n + slm_m;	/* Yes, this is correct */
			break;

		case F_2DIID:
			/*
			 * 2DIID-model; need length N
			 */
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "N");
			}
			n = iniparser_getint(ini, inla_string_join(secname, "N"), 0);
			if (n <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
			}
			if (mb->verbose) {
				printf("\t\tn=[%1d]\n", n);
			}
			Free(ptmp);
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = n;
			break;

		case F_IID1D:
		case F_IID2D:
		case F_IID3D:
		case F_IID4D:
		case F_IID5D:
		{
			/*
			 * IID_WISHART-model; need length N
			 */
			int dim = WISHART_DIM(mb->nf);
			assert(dim > 0);

			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
			if (!ptmp) {
				inla_error_missing_required_field(__GMRFLib_FuncName, secname, "N");
			}
			n = iniparser_getint(ini, inla_string_join(secname, "N"), 0);
			if (n <= 0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
			}
			if (!inla_divisible(n, dim)) {
				GMRFLib_sprintf(&msg, "%s: N=%1d is not divisible by %1d", secname, n, dim);
				inla_error_general(msg);
				exit(EXIT_FAILURE);
			}
			if (mb->verbose) {
				printf("\t\tdim=[%1d]\n", dim);
				printf("\t\tn=[%1d]\n", n);
			}
			Free(ptmp);
			mb->f_N[mb->nf] = mb->f_n[mb->nf] = n;
			break;
		}

		case F_SPDE:
			// nothing to do
			break;

		case F_SPDE2:
			// nothing to do
			break;

		case F_SPDE3:
			// nothing to do
			break;

		case F_IID:
		case F_RW1:
		case F_RW2:
		case F_CRW2:
		case F_OU:
		case F_COPY:
		case F_MEC:
		case F_MEB:
		case F_CLINEAR:
		case F_SIGM:
		case F_REVSIGM:
		case F_R_GENERIC:
			/*
			 * RW-models and OU-model and ME: read LOCATIONS, set N from LOCATIONS, else read field N and use LOCATIONS=DEFAULT.
			 */
			filename = GMRFLib_strdup(file_loc);
			if (!filename) {
				if (mb->verbose) {
					printf("\t\tfile for locations=[(NULL)]: read n...\n");
				}
				ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "N"), NULL));
				if (!ptmp) {
					GMRFLib_sprintf(&msg, "%s: section[%s]: LOCATIONS is NULL hence N is required", __GMRFLib_FuncName, secname);
					inla_error_general(msg);
				} else {
					mb->f_n[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "N"), -1);
					if (mb->f_n[mb->nf] <= 0) {
						inla_error_field_is_void(__GMRFLib_FuncName, secname, "N", ptmp);
					}
				}
				mb->f_N[mb->nf] = mb->f_n[mb->nf];
				mb->f_locations[mb->nf] = NULL;
				if (mb->verbose) {
					printf("\t\tn=[%1d]: use default locations, if required\n", mb->f_n[mb->nf]);
				}
				/*
				 * this special option is only valid for model=iid,rw1,rw2 and locations=default. therefore we do not add it
				 * into the mb->f_.... 
				 */
				mb->f_cyclic[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "CYCLIC"), 0);
				if (mb->verbose) {
					printf("\t\tcyclic=[%1d]\n", mb->f_cyclic[mb->nf]);
				}
			} else {
				if (mb->verbose) {
					printf("\t\tfile for locations=[%s]\n", filename);
				}
				inla_read_data_all(&(mb->f_locations[mb->nf]), &nlocations, filename);

				/*
				 * if N is set, make sure it match with NLOCATIONS 
				 */
				mb->f_n[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "N"), -99);
				if (mb->f_n[mb->nf] != -99 && nlocations != mb->f_n[mb->nf]) {
					GMRFLib_sprintf(&msg, "Number of locations and N does not match: %d != %d\n", nlocations, mb->f_n[mb->nf]);
					inla_error_general(msg);
					exit(EXIT_FAILURE);
				}
				mb->f_N[mb->nf] = mb->f_n[mb->nf] = nlocations;
				if (mb->verbose) {
					printf("\t\t\tnlocations=[%1d]\n", nlocations);
					for (i = 0; i < IMIN(PREVIEW, nlocations); i++) {
						printf("\t\t\tlocations[%1d]=[%g]\n", i, mb->f_locations[mb->nf][i]);
					}
				}
				/*
				 * the locations must be sorted (for some models only), otherwise, things are messed up!!!!
				 */
				if (mb->f_id[mb->nf] == F_RW1 || mb->f_id[mb->nf] == F_RW2 || mb->f_id[mb->nf] == F_CRW2 || mb->f_id[mb->nf] == F_OU) {
					for (i = 0; i < nlocations - 1; i++) {
						if (mb->f_locations[mb->nf][i] >= mb->f_locations[mb->nf][i + 1]) {
							inla_error_file_error_sorted(__GMRFLib_FuncName, filename, nlocations, i, mb->f_locations[mb->nf][i]);
						}
					}
				}
				mb->f_cyclic[mb->nf] = iniparser_getboolean(ini, inla_string_join(secname, "CYCLIC"), 0);
				if (mb->verbose) {
					printf("\t\tcyclic=[%1d]\n", mb->f_cyclic[mb->nf]);
				}
			}
			break;

		default:
			/*
			 * if this happens, its an error, as I have forgot one of the models in the list....
			 */
			GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
			break;
		}
	}


	// read locations also here if not read before
	if (!mb->f_locations[mb->nf] && mb->f_n[mb->nf] > 0) {
		filename = GMRFLib_strdup(file_loc);
		if (filename)
			if (mb->verbose) {
				printf("\t\tfile for locations=[%s]\n", filename);
			}
		inla_read_data_all(&(mb->f_locations[mb->nf]), &nlocations, filename);
		if (mb->f_N[mb->nf] > nlocations) {
			double *t = Calloc(mb->f_N[mb->nf], double);
			memcpy(t, mb->f_locations[mb->nf], nlocations * sizeof(double));
			Free(mb->f_locations[mb->nf]);
			mb->f_locations[mb->nf] = t;
			for (i = nlocations; i < mb->f_N[mb->nf]; i++) {
				mb->f_locations[mb->nf][i] = i + 1;
			}
		}
		if (nlocations != mb->f_n[mb->nf]) {
			GMRFLib_sprintf(&msg, "Number of locations and N does not match: %d != %d\n", nlocations, mb->f_n[mb->nf]);
			inla_error_general(msg);
			exit(EXIT_FAILURE);
		}
		if (mb->verbose) {
			printf("\t\t\tnlocations=[%1d]\n", nlocations);
			for (i = 0; i < IMIN(PREVIEW, nlocations); i++) {
				printf("\t\t\tlocations[%1d]=[%g]\n", i, mb->f_locations[mb->nf][i]);
			}
		}
	}

	switch (mb->f_id[mb->nf]) {
	case F_RW2D:
	case F_BESAG:
	case F_GENERIC0:
	case F_SEASONAL:
	case F_IID:
	case F_RW1:
	case F_RW2:
	case F_CRW2:
	case F_Z:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf] = Calloc(1, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_SPDE:
	{
		char *spde_prefix;
		int nT, nK;

		spde_prefix = GMRFLib_strdup(".");
		spde_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE_PREFIX"), spde_prefix);
		spde_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE.PREFIX"), spde_prefix);
		spde_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDEPREFIX"), spde_prefix);
		if (mb->verbose) {
			printf("\t\tspde.prefix = [%s]\n", spde_prefix);
		}

		/*
		 * 
		 */
		inla_spde_build_model(&spde_model_orig, (const char *) spde_prefix);
		mb->f_model[mb->nf] = (void *) spde_model_orig;

		/*
		 * The _userfunc0 must be set directly after the _build_model() call. This is a bit dirty; FIXME later. 
		 */
		inla_spde_build_model(&spde_model, (const char *) spde_prefix);
		GMRFLib_ai_INLA_userfunc0 = (GMRFLib_ai_INLA_userfunc0_tp *) inla_spde_userfunc0;
		GMRFLib_ai_INLA_userfunc1 = (GMRFLib_ai_INLA_userfunc1_tp *) inla_spde_userfunc1;
		// GMRFLib_ai_INLA_userfunc0_dim = mb->ntheta; NOT NEEDED; set in userfunc0
		GMRFLib_ai_INLA_userfunc1_dim = mb->ntheta;    /* this is a hack and gives the offset of theta... */

		double initial_t = 0.0, initial_k = 0.0, initial_rest = 0.0, initial_oc = 0.0;

		/*
		 * reread this here, as we need non-std defaults 
		 */
		mb->f_fixed[mb->nf][3] = iniparser_getboolean(ini, inla_string_join(secname, "FIXED3"), 1);	/* default fixed, yes */

		// P(mb->nf);
		// P(mb->f_fixed[mb->nf][3]);

		initial_t = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 0.0);
		initial_k = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		initial_rest = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), 0.0);
		initial_oc = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL3"), -20.0);
		SetInitial(0, initial_t);
		SetInitial(1, initial_k);
		SetInitial(2, initial_rest);
		SetInitial(3, initial_oc);

		nT = spde_model->Tmodel->ntheta;
		nK = spde_model->Kmodel->ntheta;
		mb->f_ntheta[mb->nf] = IMAX(4, nT + nK + 1);
		mb->f_Tmodel[mb->nf] = GMRFLib_strdup("basisT");
		mb->f_Kmodel[mb->nf] = GMRFLib_strdup("basisK");

		if (mb->verbose) {
			printf("\t\tnT=[%d]\n", nT);
			printf("\t\tnK=[%d]\n", nK);
			printf("\t\tinitialise theta_t=[%g]\n", initial_t);
			printf("\t\tinitialise theta_k=[%g]\n", initial_k);
			printf("\t\tinitialise theta_rest=[%g]\n", initial_rest);
			printf("\t\tinitialise theta_oc=[%g]\n", initial_oc);
			printf("\t\tfixed_t=[%1d]\n", mb->f_fixed[mb->nf][0]);
			printf("\t\tfixed_k=[%1d]\n", mb->f_fixed[mb->nf][1]);
			printf("\t\tfixed_rest=[%1d]\n", mb->f_fixed[mb->nf][2]);
			printf("\t\tfixed_oc=[%1d]\n", mb->f_fixed[mb->nf][3]);
		}

		for (k = 0; k < nT; k++) {
			if (k == 0) {
				if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				} else {
					tmp = initial_t;
				}
			} else {
				if (!mb->f_fixed[mb->nf][2] && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				} else {
					tmp = initial_rest;
				}
			}
			HYPER_INIT(spde_model->Tmodel->theta[k], tmp);
		}
		for (k = 0; k < nK; k++) {
			if (k == 0) {
				if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				} else {
					tmp = initial_k;
				}
			} else {
				if (!mb->f_fixed[mb->nf][2] && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				} else {
					tmp = initial_rest;
				}
			}
			HYPER_INIT(spde_model->Kmodel->theta[k], tmp);
		}
		if (!mb->f_fixed[mb->nf][3] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		} else {
			tmp = initial_oc;
		}
		HYPER_INIT(spde_model->oc, tmp);

		mb->f_theta[mb->nf] = Calloc(nT + nK + 1, double **);
		for (k = 0; k < nT; k++) {
			mb->f_theta[mb->nf][k] = spde_model->Tmodel->theta[k];
		}
		for (k = 0; k < nK; k++) {
			mb->f_theta[mb->nf][k + nT] = spde_model->Kmodel->theta[k];
		}
		mb->f_theta[mb->nf][nK + nT] = spde_model->oc;

		for (k = 0; k < nT + nK + 1; k++) {
			int fx;

			if (k == 0) {
				fx = mb->f_fixed[mb->nf][0];   /* T[0] */
			} else if (k == nT) {
				fx = mb->f_fixed[mb->nf][1];   /* K[0] */
			} else if (k == nT + nK) {
				fx = mb->f_fixed[mb->nf][3];   /* oc */
			} else {
				fx = mb->f_fixed[mb->nf][2];   /* the rest */
			}

			if (!fx) {
				/*
				 * add this \theta 
				 */
				mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
				mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
				mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
				mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

				if (k == nT + nK) {
					GMRFLib_sprintf(&msg, "%s for %s", "Oc", (secname ? secname : mb->f_tag[mb->nf]));
				} else {
					if (k < nT) {
						GMRFLib_sprintf(&msg, "%s.%1d for %s-%s", "T", k, (secname ? secname : mb->f_tag[mb->nf]), mb->f_Tmodel[mb->nf]);
					} else {
						GMRFLib_sprintf(&msg, "%s.%1d for %s-%s", "K", k - nT,
								(secname ? secname : mb->f_tag[mb->nf]), mb->f_Kmodel[mb->nf]);
					}
				}
				mb->theta_tag[mb->ntheta] = msg;
				mb->theta_tag_userscale[mb->ntheta] = msg;

				if (k == nT + nK) {
					GMRFLib_sprintf(&msg, "%s-parameter-Oc", mb->f_dir[mb->nf]);
				} else {
					if (k < nT) {
						GMRFLib_sprintf(&msg, "%s-parameter-T.%1d-%s", mb->f_dir[mb->nf], k, mb->f_Tmodel[mb->nf]);
					} else {
						GMRFLib_sprintf(&msg, "%s-parameter-K.%1d-%s", mb->f_dir[mb->nf], k - nT, mb->f_Kmodel[mb->nf]);
					}
				}
				mb->theta_dir[mb->ntheta] = msg;

				if (k == nT + nK) {
					mb->theta[mb->ntheta] = spde_model->oc;
				} else {
					if (k < nT) {
						mb->theta[mb->ntheta] = spde_model->Tmodel->theta[k];
					} else {
						mb->theta[mb->ntheta] = spde_model->Kmodel->theta[k - nT];
					}
				}

				mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
				if (k == nT + nK) {
					mb->theta_map[mb->ntheta] = map_probability;
				} else {
					mb->theta_map[mb->ntheta] = map_identity;
				}
				mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
				mb->theta_map_arg[mb->ntheta] = NULL;

				mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
				mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);

				int pri;
				if (k == 0) {
					pri = 0;
				} else if (k == nT) {
					pri = 1;
				} else if (k == nT + nK) {
					pri = 3;
				} else {
					pri = 2;
				}

				mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][pri].from_theta);
				mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][pri].to_theta);

				mb->ntheta++;
			}
		}
		break;
	}

	case F_SPDE2:
	{
		char *spde2_prefix, *transform;

		spde2_prefix = GMRFLib_strdup(".");
		spde2_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE2_PREFIX"), spde2_prefix);
		spde2_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE2.PREFIX"), spde2_prefix);
		spde2_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE2PREFIX"), spde2_prefix);
		if (mb->verbose) {
			printf("\t\tspde2.prefix = [%s]\n", spde2_prefix);
		}

		transform = GMRFLib_strdup("logit");
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE2_TRANSFORM"), transform);
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE2.TRANSFORM"), transform);
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE2TRANSFORM"), transform);
		if (mb->verbose) {
			printf("\t\tspde2.transform = [%s]\n", transform);
		}

		/*
		 * need to read this twice. can save memory by changing the pointer from spde2_model_orig to spde2_model, like for B and M matrices and BLC. maybe
		 * do later 
		 */
		inla_spde2_build_model(&spde2_model_orig, (const char *) spde2_prefix, (const char *) transform);
		mb->f_model[mb->nf] = (void *) spde2_model_orig;

		inla_spde2_build_model(&spde2_model, (const char *) spde2_prefix, (const char *) transform);

		/*
		 * set up userfunc2 that computes the marginal of BLC*theta.intern 
		 */
		GMRFLib_ai_INLA_userfunc2_n++;

		GMRFLib_ai_INLA_userfunc2_args = Realloc(GMRFLib_ai_INLA_userfunc2_args, GMRFLib_ai_INLA_userfunc2_n, void *);
		GMRFLib_ai_INLA_userfunc2_args[GMRFLib_ai_INLA_userfunc2_n - 1] = (void *) spde2_model;
		GMRFLib_ai_INLA_userfunc2 = Realloc(GMRFLib_ai_INLA_userfunc2, GMRFLib_ai_INLA_userfunc2_n, GMRFLib_ai_INLA_userfunc2_tp *);
		GMRFLib_ai_INLA_userfunc2[GMRFLib_ai_INLA_userfunc2_n - 1] = (GMRFLib_ai_INLA_userfunc2_tp *) inla_spde2_userfunc2;

		char *ltag;
		GMRFLib_sprintf(&ltag, "%s", secname);
		GMRFLib_ai_INLA_userfunc2_tag = Realloc(GMRFLib_ai_INLA_userfunc2_tag, GMRFLib_ai_INLA_userfunc2_n, char *);
		GMRFLib_ai_INLA_userfunc2_tag[GMRFLib_ai_INLA_userfunc2_n - 1] = ltag;

		/*
		 * now we know the number of hyperparameters ;-) 
		 */
		int ntheta;

		mb->f_ntheta[mb->nf] = ntheta = spde2_model->ntheta;
		mb->f_initial[mb->nf] = Calloc(mb->f_ntheta[mb->nf], double);	/* need to do this here as we do not know n_theta upfront */
		if (mb->verbose) {
			printf("\t\tntheta = [%1d]\n", ntheta);
		}

		if (!strcasecmp(mb->f_prior[mb->nf][0].name, "MVNORM")) {
			if ((int) mb->f_prior[mb->nf][0].parameters[0] != ntheta) {
				GMRFLib_sprintf(&ptmp, "Dimension of the MVNORM prior is not equal to number of hyperparameters: %1d != %1d\n",
						(int) mb->f_prior[mb->nf][0].parameters[0], ntheta);
				inla_error_general(ptmp);
				exit(EXIT_FAILURE);
			}
		} else if (!strcasecmp(mb->f_prior[mb->nf][0].name, "PCSPDEGA")) {
			assert(ntheta == 2);		       /* requirement... */
		} else {
			assert(0 == 1);			       /* wrong prior used... */
		}


		mb->f_fixed[mb->nf] = Calloc(ntheta, int);
		mb->f_theta[mb->nf] = Calloc(ntheta, double **);

		/*
		 * mark all possible as read 
		 */
		for (i = 0; i < SPDE2_MAXTHETA; i++) {
			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PRIOR%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PARAMETERS%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "to.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "from.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);
		}

		/*
		 * need to know where in the theta-list the spde2 parameters are 
		 */
		spde2_model->theta_first_idx = mb->ntheta;

		/*
		 * then read those we need 
		 */
		for (i = 0; i < ntheta; i++) {
			double theta_initial = 0.0;

			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			mb->f_fixed[mb->nf][i] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);
			if (mb->f_fixed[mb->nf][i]) {
				inla_error_general("Fixed hyperparmaters is not allowed in the SPDE2 model.");
				exit(EXIT_FAILURE);
			}

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			theta_initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), theta_initial);
			if (!mb->f_fixed[mb->nf][i] && mb->reuse_mode) {
				theta_initial = mb->theta_file[mb->theta_counter_file++];
			}

			HYPER_INIT(spde2_model->theta[i], theta_initial);

			if (mb->verbose) {
				printf("\t\tinitialise theta[%1d]=[%g]\n", i, theta_initial);
				printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][i]);
			}

			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], i + 1);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);	/* YES, use prior0 */
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);	/* YES, use prior0 */

			mb->theta[mb->ntheta] = spde2_model->theta[i];
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_SPDE3:
	{
		char *spde3_prefix, *transform;

		spde3_prefix = GMRFLib_strdup(".");
		spde3_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE3_PREFIX"), spde3_prefix);
		spde3_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE3.PREFIX"), spde3_prefix);
		spde3_prefix = iniparser_getstring(ini, inla_string_join(secname, "SPDE3PREFIX"), spde3_prefix);
		if (mb->verbose) {
			printf("\t\tspde3.prefix = [%s]\n", spde3_prefix);
		}

		transform = GMRFLib_strdup("logit");
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE3_TRANSFORM"), transform);
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE3.TRANSFORM"), transform);
		transform = iniparser_getstring(ini, inla_string_join(secname, "SPDE3TRANSFORM"), transform);
		if (mb->verbose) {
			printf("\t\tspde3.transform = [%s]\n", transform);
		}

		/*
		 * need to read this twice. can save memory by changing the pointer from spde3_model_orig to spde3_model, like for B and M matrices and
		 * BLC. maybe do later
		 */
		inla_spde3_build_model(&spde3_model_orig, (const char *) spde3_prefix, (const char *) transform);
		mb->f_model[mb->nf] = (void *) spde3_model_orig;

		inla_spde3_build_model(&spde3_model, (const char *) spde3_prefix, (const char *) transform);

		/*
		 * set up userfunc3 that computes the marginal of BLC*theta.intern 
		 */
		GMRFLib_ai_INLA_userfunc3_n++;

		GMRFLib_ai_INLA_userfunc3_args = Realloc(GMRFLib_ai_INLA_userfunc3_args, GMRFLib_ai_INLA_userfunc3_n, void *);
		GMRFLib_ai_INLA_userfunc3_args[GMRFLib_ai_INLA_userfunc3_n - 1] = (void *) spde3_model;
		GMRFLib_ai_INLA_userfunc3 = Realloc(GMRFLib_ai_INLA_userfunc3, GMRFLib_ai_INLA_userfunc3_n, GMRFLib_ai_INLA_userfunc3_tp *);
		GMRFLib_ai_INLA_userfunc3[GMRFLib_ai_INLA_userfunc3_n - 1] = (GMRFLib_ai_INLA_userfunc3_tp *) inla_spde3_userfunc3;

		char *ltag;
		GMRFLib_sprintf(&ltag, "%s", secname);
		GMRFLib_ai_INLA_userfunc3_tag = Realloc(GMRFLib_ai_INLA_userfunc3_tag, GMRFLib_ai_INLA_userfunc3_n, char *);
		GMRFLib_ai_INLA_userfunc3_tag[GMRFLib_ai_INLA_userfunc3_n - 1] = ltag;

		/*
		 * now we know the number of hyperparameters ;-) 
		 */
		int ntheta;

		mb->f_ntheta[mb->nf] = ntheta = spde3_model->ntheta;
		mb->f_initial[mb->nf] = Calloc(mb->f_ntheta[mb->nf], double);	/* need to do this here as we do not know n_theta upfront */
		if (mb->verbose) {
			printf("\t\tntheta = [%1d]\n", ntheta);
		}

		if ((int) mb->f_prior[mb->nf][0].parameters[0] != ntheta) {
			GMRFLib_sprintf(&ptmp, "Dimension of the MVNORM prior is not equal to number of hyperparameters: %1d != %1d\n",
					(int) mb->f_prior[mb->nf][0].parameters[0], ntheta);
			inla_error_general(ptmp);
			exit(EXIT_FAILURE);
		}

		mb->f_fixed[mb->nf] = Calloc(ntheta, int);
		mb->f_theta[mb->nf] = Calloc(ntheta, double **);

		/*
		 * mark all possible as read 
		 */
		for (i = 0; i < SPDE3_MAXTHETA; i++) {
			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PRIOR%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PARAMETERS%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "to.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "from.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);
		}

		/*
		 * need to know where in the theta-list the spde3 parameters are 
		 */
		spde3_model->theta_first_idx = mb->ntheta;

		/*
		 * then read those we need 
		 */
		for (i = 0; i < ntheta; i++) {
			double theta_initial = 0.0;

			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			mb->f_fixed[mb->nf][i] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);
			if (mb->f_fixed[mb->nf][i]) {
				inla_error_general("Fixed hyperparmaters is not allowed in the SPDE3 model.");
				exit(EXIT_FAILURE);
			}

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			theta_initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), theta_initial);
			if (!mb->f_fixed[mb->nf][i] && mb->reuse_mode) {
				theta_initial = mb->theta_file[mb->theta_counter_file++];
			}

			HYPER_INIT(spde3_model->theta[i], theta_initial);

			if (mb->verbose) {
				printf("\t\tinitialise theta[%1d]=[%g]\n", i, theta_initial);
				printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][i]);
			}

			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], i + 1);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);	/* YES, use prior0 */
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);	/* YES, use prior0 */

			mb->theta[mb->ntheta] = spde3_model->theta[i];
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_AR:
	{
		int ntheta;

		ntheta = mb->f_ntheta[mb->nf] = mb->f_order[mb->nf] + 1;
		assert(ntheta <= AR_MAXTHETA + 1);
		mb->f_initial[mb->nf] = Calloc(ntheta, double);
		if (mb->verbose) {
			printf("\t\tntheta = [%1d]\n", ntheta);
		}
		mb->f_prior[mb->nf][1].parameters[1] = ntheta - 1;	/* add the order as the second parameter in the pc-prior! */

		/*
		 * mark all possible as read 
		 */
		for (i = 0; i < AR_MAXTHETA + 1; i++) {

			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PRIOR%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "PARAMETERS%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "to.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

			GMRFLib_sprintf(&ctmp, "from.theta%1d", i);
			iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);
		}

		mb->f_fixed[mb->nf] = Calloc(ntheta, int);
		mb->f_theta[mb->nf] = Calloc(AR_MAXTHETA + 1, double **);

		HYPER_NEW(log_prec, 0.0);
		mb->f_theta[mb->nf][0] = log_prec;
		pacf_intern = Calloc(AR_MAXTHETA, double **);
		for (i = 0; i < AR_MAXTHETA; i++) {
			HYPER_NEW(pacf_intern[i], 0.0);
			mb->f_theta[mb->nf][i + 1] = pacf_intern[i];
		}

		/*
		 * then read those we need 
		 */
		for (i = 0; i < ntheta; i++) {
			double theta_initial = 0.0;

			GMRFLib_sprintf(&ctmp, "FIXED%1d", i);
			mb->f_fixed[mb->nf][i] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);

			GMRFLib_sprintf(&ctmp, "INITIAL%1d", i);
			theta_initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), theta_initial);
			if (!mb->f_fixed[mb->nf][i] && mb->reuse_mode) {
				theta_initial = mb->theta_file[mb->theta_counter_file++];
			}

			if (i == 0) {
				/*
				 * precision 
				 */
				HYPER_INIT(log_prec, theta_initial);
				if (mb->verbose) {
					printf("\t\tinitialise (log_prec) theta[%1d]=[%g]\n", i, theta_initial);
					printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][i]);
				}

				if (!mb->f_fixed[mb->nf][i]) {
					/*
					 * add this \theta 
					 */
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
					GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));

					mb->theta_tag[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag_userscale[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], i + 1);
					mb->theta_dir[mb->ntheta] = msg;

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);
					mb->theta[mb->ntheta] = log_prec;
					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map[mb->ntheta] = map_precision;
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
					mb->theta_map_arg[mb->ntheta] = NULL;
					mb->ntheta++;
				}
			} else {
				/*
				 * PACF 
				 */
				HYPER_INIT(pacf_intern[i - 1], theta_initial);
				if (mb->verbose) {
					printf("\t\tinitialise (PACF) theta[%1d]=[%g]\n", i, theta_initial);
					printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][i]);
				}
				if (!mb->f_fixed[mb->nf][i]) {
					/*
					 * add this \theta 
					 */
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
					GMRFLib_sprintf(&msg, "Intern PACF%1d for %s", i, (secname ? secname : mb->f_tag[mb->nf]));

					mb->theta_tag[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "PACF%1d for %s", i, (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag_userscale[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], i + 1);
					mb->theta_dir[mb->ntheta] = msg;

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);
					mb->theta[mb->ntheta] = pacf_intern[i - 1];
					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map[mb->ntheta] = map_phi;
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
					mb->theta_map_arg[mb->ntheta] = NULL;
					mb->ntheta++;
				}
			}
		}
		break;
	}

	case F_MEC:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 1.0);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(beta, tmp);

		double *range = NULL;
		range = Calloc(2, double);		       /* need this as it will be stored in the map argument */
		range[0] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.LOW"), 0.0);	/* low = high ==> map = identity */
		range[1] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.HIGH"), 0.0);

		if (mb->verbose) {
			printf("\t\tinitialise beta[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
			printf("\t\trange[%g, %g]\n", range[0], range[1]);
		}

		mb->f_theta_map[mb->nf] = Calloc(1, map_func_tp *);
		mb->f_theta_map_arg[mb->nf] = Calloc(1, void *);
		mb->f_theta_map[mb->nf][0] = map_beta;	       /* need these */
		mb->f_theta_map_arg[mb->nf][0] = (void *) range;	/* and this one as well */
		mb->f_theta[mb->nf][0] = beta;

		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEC beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEC beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = beta;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_beta;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = (void *) range;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_prec_u[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = log_prec;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEC prec_u_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEC prec_u for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), 0.0);
		if (!mb->f_fixed[mb->nf][2] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(2, tmp);
		HYPER_INIT(mean_x, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise mean_x[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][2]);
		}

		mb->f_theta[mb->nf][2] = mean_x;
		if (!mb->f_fixed[mb->nf][2]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEC mean_x for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEC mean_x for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter2", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].to_theta);

			mb->theta[mb->ntheta] = mean_x;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL3"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][3] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(3, tmp);
		HYPER_INIT(log_prec_x, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_prec_x[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][3]);
		}
		mb->f_theta[mb->nf][3] = log_prec_x;
		if (!mb->f_fixed[mb->nf][3]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEC prec_x_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEC prec_x for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter3", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][3].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][3].to_theta);

			mb->theta[mb->ntheta] = log_prec_x;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_MEB:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 1.0);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(beta, tmp);

		double *range = NULL;
		range = Calloc(2, double);		       /* need this as it will be stored in the map argument */
		range[0] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.LOW"), 0.0);	/* low = high ==> map = identity */
		range[1] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.HIGH"), 0.0);

		if (mb->verbose) {
			printf("\t\tinitialise beta[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
			printf("\t\trange[%g, %g]\n", range[0], range[1]);
		}

		mb->f_theta_map[mb->nf] = Calloc(1, map_func_tp *);
		mb->f_theta_map_arg[mb->nf] = Calloc(1, void *);
		mb->f_theta_map[mb->nf][0] = map_beta;	       /* need these */
		mb->f_theta_map_arg[mb->nf][0] = (void *) range;	/* and this one as well */
		mb->f_theta[mb->nf][0] = beta;

		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEB beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEB beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = beta;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_beta;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = (void *) range;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_prec_u[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = log_prec;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "MEB prec_u_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "MEB prec_u for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_R_GENERIC:
	{
#if defined(WINDOWS)
		inla_error_general("Model 'rgeneric' is not available for Windows; please use Linux or MacOSX.");
		exit(EXIT_FAILURE);
#else
		int err, debug = 0;

		Id = iniparser_getint(ini, inla_string_join(secname, "RGENERIC.ID"), -999);
		filename_R2c = iniparser_getstring(ini, inla_string_join(secname, "RGENERIC.FIFO.R2c"), NULL);
		filename_c2R = iniparser_getstring(ini, inla_string_join(secname, "RGENERIC.FIFO.c2R"), NULL);

		if (mb->verbose) {
			printf("\t\trgeneric.Id [%1d]\n", Id);
			printf("\t\trgeneric.FIFO.R2c [%s]\n", filename_R2c);
			printf("\t\trgeneric.FIFO.c2R [%s]\n", filename_c2R);
		}

		if (debug) {
			printf("C open R2c %s\n", filename_R2c);
		}
		err = mkfifo(filename_R2c, 0700);
		assert(err == 0);
		R2c = open(filename_R2c, O_RDONLY);
		if (debug) {
			printf("C open R2c done\n");
			printf("C open c2R %s\n", filename_c2R);
		}
		err = mkfifo(filename_c2R, 0700);
		assert(err == 0);
		c2R = open(filename_c2R, O_WRONLY);
		if (debug) {
			printf("C open c2R done\n");
		}

		int ntheta;
		double *initial = NULL;
#pragma omp critical
		{
			WRITE_MSG(c2R, Id, R_GENERIC_INITIAL);
			READ(R2c, &ntheta, 1, int);
			if (ntheta) {
				initial = Calloc(ntheta, double);
			}
			READ(R2c, initial, ntheta, double);
		}

		mb->f_ntheta[mb->nf] = ntheta;
		mb->f_initial[mb->nf] = initial;
		if (mb->verbose) {
			int ii;

			printf("\t\tntheta = [%1d]\n", ntheta);
			for (ii = 0; ii < ntheta; ii++) {
				printf("\t\tinitial[%1d] = %g\n", ii, initial[ii]);
			}
		}

		mb->f_fixed[mb->nf] = Calloc(ntheta, int);
		mb->f_theta[mb->nf] = Calloc(ntheta, double **);

		for (i = 0; i < ntheta; i++) {
			double theta_initial;

			mb->f_fixed[mb->nf][i] = 0;
			theta_initial = initial[i];

			if (!mb->f_fixed[mb->nf][i] && mb->reuse_mode) {
				theta_initial = mb->theta_file[mb->theta_counter_file++];
			}
			HYPER_NEW(mb->f_theta[mb->nf][i], theta_initial);
			if (mb->verbose) {
				printf("\t\tinitialise theta[%1d]=[%g]\n", i, theta_initial);
				printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][i]);
			}

			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Theta%1d for %s", i + 1, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], i + 1);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup("function(x) x");
			mb->theta_to[mb->ntheta] = GMRFLib_strdup("function(x) x");

			mb->theta[mb->ntheta] = mb->f_theta[mb->nf][i];
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

#endif							       /* !defined(WINDOWS) */
		break;
	}

	case F_AR1:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(phi_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise phi_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = phi_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Rho_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Rho for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = phi_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_rho;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_SLM:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(rho_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise rho_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = rho_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Rho_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Rho for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = rho_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_OU:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(phi_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise phi_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = phi_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Phi_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Phi for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = phi_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_BESAG2:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(a_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise a_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = a_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "a_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "a for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = a_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_BESAGPROPER:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_diag, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log weight[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = log_diag;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log diagonal for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Diagonal for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_diag;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_exp;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_BESAGPROPER2:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(phi_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise logit lambda[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = phi_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Logit lambda for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Lambda for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = phi_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_GENERIC1:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(beta_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise beta_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = beta_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Beta_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = beta_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_GENERIC2:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision-cmatrix for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision-cmatrix for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(h2_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise h2-intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = h2_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "h2-intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "h2 for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = h2_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_GENERIC3:
	{
		inla_generic3_tp *a = (inla_generic3_tp *) mb->f_Qfunc_arg[mb->nf];
		a->log_prec = Calloc(GENERIC3_MAXTHETA, double **);
		mb->f_theta[mb->nf] = Calloc(GENERIC3_MAXTHETA, double **);

		for (k = a->m; k < GENERIC3_MAXTHETA - 1; k++) {	/* yes, do not include the common scaling */
			mb->f_fixed[mb->nf][k] = 1;	       /* those not used are set to fixed */
		}
		for (k = 0; k < GENERIC3_MAXTHETA; k++) {
			GMRFLib_sprintf(&ctmp, "INITIAL%1d", k);
			tmp = iniparser_getdouble(ini, inla_string_join(secname, ctmp), G.log_prec_initial);
			if (!mb->f_fixed[mb->nf][k] && mb->reuse_mode) {
				tmp = mb->theta_file[mb->theta_counter_file++];
			}
			SetInitial(k, tmp);
			HYPER_NEW(a->log_prec[k], tmp);
			if (mb->verbose) {
				printf("\t\tinitialise log_precision[%1d] = %g\n", k, tmp);
				printf("\t\tfixed[%1d] = %1d\n", k, mb->f_fixed[mb->nf][k]);
			}
			mb->f_theta[mb->nf][k] = a->log_prec[k];

			if (!mb->f_fixed[mb->nf][k]) {
				/*
				 * add this \theta 
				 */
				mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
				mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
				mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
				mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
				if (k < GENERIC3_MAXTHETA - 1) {
					GMRFLib_sprintf(&msg, "Log precision for Cmatrix[[%1d]] for %s", k + 1, (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "Precision for Cmatrix[[%1d]] for %s", k + 1, (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag_userscale[mb->ntheta] = msg;
				} else {
					GMRFLib_sprintf(&msg, "Log common precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "Common precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
					mb->theta_tag_userscale[mb->ntheta] = msg;
				}
				GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], k);
				mb->theta_dir[mb->ntheta] = msg;

				mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
				mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
				mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].from_theta);
				mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].to_theta);

				mb->theta[mb->ntheta] = a->log_prec[k];
				mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
				mb->theta_map[mb->ntheta] = map_precision;
				mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
				mb->theta_map_arg[mb->ntheta] = NULL;
				mb->ntheta++;
			}
		}
		break;
	}

	case F_COPY:
	{
		mb->f_of[mb->nf] = iniparser_getstring(ini, inla_string_join(secname, "OF"), NULL);
		if (mb->verbose && mb->f_of[mb->nf]) {
			printf("\t\tof=[%s]\n", mb->f_of[mb->nf]);
		}

		/*
		 * same_as, says that the beta-parameters is the same as 'same_as', so this is to be determined later on. so we need to add space for it in
		 * f_theta, but not in mb->theta and mb->ntheta. The error-checking is done later. 
		 */
		mb->f_same_as[mb->nf] = iniparser_getstring(ini, inla_string_join(secname, "SAMEAS"), NULL);
		mb->f_same_as[mb->nf] = iniparser_getstring(ini, inla_string_join(secname, "SAME.AS"), mb->f_same_as[mb->nf]);
		if (mb->verbose) {
			printf("\t\tsame.as=[%s]\n", mb->f_same_as[mb->nf]);
		}

		mb->f_precision[mb->nf] = iniparser_getdouble(ini, inla_string_join(secname, "PRECISION"), mb->f_precision[mb->nf]);
		if (mb->verbose) {
			printf("\t\tprecision=[%f]\n", mb->f_precision[mb->nf]);
		}

		int fixed_default = -1;
		fixed_default = iniparser_getint(ini, inla_string_join(secname, "FIXED"), fixed_default);
		if (fixed_default == -1) {
			mb->f_fixed[mb->nf][0] = 1;
		}
		if (mb->verbose && fixed_default == -1) {
			printf("\t\tfixed=[%d]\n", mb->f_fixed[mb->nf][0]);
		}

		double *range = NULL;
		range = Calloc(2, double);		       /* need this as it will be stored in the map argument */
		range[0] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.LOW"), 0.0);	/* low = high ==> map = identity */
		range[1] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.HIGH"), 0.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 1.0);	/* yes! default value is 1 */
		if (tmp == 0.0) {
			inla_error_general("The initial value for the scaling (beta) in a copy-model, cannot be zero");
			assert(tmp != 0.0);
			exit(1);
		}
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(beta, tmp);
		if (mb->verbose) {
			printf("\t\trange[%g, %g]\n", range[0], range[1]);
			printf("\t\tinitialise beta[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(1, double **);
		mb->f_theta[mb->nf][0] = beta;

		mb->f_theta_map[mb->nf] = Calloc(1, map_func_tp *);
		mb->f_theta_map_arg[mb->nf] = Calloc(1, void *);
		mb->f_theta_map[mb->nf][0] = map_beta;	       /* need these */
		mb->f_theta_map_arg[mb->nf][0] = (void *) range;	/* and this one as well */

		if (mb->f_same_as[mb->nf] == NULL && !mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Beta_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = beta;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_beta;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = (void *) range;
			mb->ntheta++;
		}
		break;
	}

	case F_CLINEAR:
	{
		mb->f_precision[mb->nf] = iniparser_getdouble(ini, inla_string_join(secname, "PRECISION"), mb->f_precision[mb->nf]);
		if (mb->verbose) {
			printf("\t\tprecision=[%f]\n", mb->f_precision[mb->nf]);
		}

		int fixed_default = 0;
		fixed_default = iniparser_getint(ini, inla_string_join(secname, "FIXED"), fixed_default);
		if (fixed_default == -1) {
			mb->f_fixed[mb->nf][0] = 1;
		}
		if (mb->verbose && fixed_default == -1) {
			printf("\t\tfixed=[%d]\n", mb->f_fixed[mb->nf][0]);
		}

		double *range = NULL;
		range = Calloc(2, double);		       /* need this as it will be stored in the map argument */
		range[0] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.LOW"), 0.0);	/* low = high ==> map = identity */
		range[1] = iniparser_getdouble(ini, inla_string_join(secname, "RANGE.HIGH"), 0.0);

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL"), 0.0);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(beta, tmp);
		if (mb->verbose) {
			printf("\t\trange[%g, %g]\n", range[0], range[1]);
			printf("\t\tinitialise beta[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		mb->f_theta[mb->nf] = Calloc(1, double **);
		mb->f_theta[mb->nf][0] = beta;

		mb->f_theta_map[mb->nf] = Calloc(1, map_func_tp *);
		mb->f_theta_map_arg[mb->nf] = Calloc(1, void *);
		mb->f_theta_map[mb->nf][0] = map_beta;	       /* need these */
		mb->f_theta_map_arg[mb->nf][0] = (void *) range;	/* and this one as well */

		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Beta_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Beta for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = beta;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_beta;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = (void *) range;
			mb->ntheta++;
		}
		break;
	}

	case F_SIGM:
	case F_REVSIGM:
	{
		char *local_name = (mb->f_id[mb->nf] == F_SIGM ? GMRFLib_strdup("SIGM") : GMRFLib_strdup("REVSIGM"));

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), 1.0);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(beta, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise beta[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf][0] = beta;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "%s beta for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s beta for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = beta;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_identity;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), log(20.0));
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_halflife, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_halflife[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf][1] = log_halflife;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

			GMRFLib_sprintf(&msg, "%s log_halflife for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s halflife for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_halflife;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), log(1.0));
		if (!mb->f_fixed[mb->nf][2] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(2, tmp);
		HYPER_INIT(log_shape, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_shape[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][2]);
		}
		mb->f_theta[mb->nf][2] = log_shape;
		if (!mb->f_fixed[mb->nf][2]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

			GMRFLib_sprintf(&msg, "%s log_shape for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s shape for %s", local_name, (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].to_theta);

			mb->theta[mb->ntheta] = log_shape;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		break;
	}


	case F_BYM:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec0, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision (iid component)[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_prec1, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision (spatial component)[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec0;
		if (!mb->f_fixed[mb->nf][0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s (idd component)", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s (iid component)", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec0;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		mb->f_theta[mb->nf][1] = log_prec1;
		if (!mb->f_fixed[mb->nf][1]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s (spatial component)", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s (spatial component)", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_prec1;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_BYM2:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec0, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision [%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(phi_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise phi_intern [%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec0;
		mb->f_theta[mb->nf][1] = phi_intern;

		if (!mb->f_fixed[mb->nf][0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec0;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		if (!mb->f_fixed[mb->nf][1]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Logit phi for %s", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Phi for %s", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = phi_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_RW2DIID:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision [%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 0.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(phi_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise phi_intern [%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}
		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		mb->f_theta[mb->nf][1] = phi_intern;

		if (!mb->f_fixed[mb->nf][0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		if (!mb->f_fixed[mb->nf][1]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Logit phi for %s", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Phi for %s", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = phi_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_probability;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_2DIID:
	{
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec0, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision (first component)[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(log_prec1, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision (second component)[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}

		mb->f_theta[mb->nf] = Calloc(3, double **);
		mb->f_theta[mb->nf][0] = log_prec0;
		if (!mb->f_fixed[mb->nf][0]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s (first component)", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s (first component)", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec0;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		mb->f_theta[mb->nf][1] = log_prec1;
		if (!mb->f_fixed[mb->nf][1]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s (second component)", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s (second component)", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = log_prec1;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL2"), 0.0);
		if (!mb->f_fixed[mb->nf][2] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(2, tmp);
		HYPER_INIT(rho_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise rho_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][2]);
		}

		mb->f_theta[mb->nf][2] = rho_intern;
		if (!mb->f_fixed[mb->nf][2]) {
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Rho_intern for %s", mb->f_tag[mb->nf]);
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Rho for %s", mb->f_tag[mb->nf]);
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter2", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][2].to_theta);

			mb->theta[mb->ntheta] = rho_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_rho;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	case F_IID1D:
	case F_IID2D:
	case F_IID3D:
	case F_IID4D:
	case F_IID5D:
	{
		int dim = WISHART_DIM(mb->nf);
		assert(dim > 0);

		int n_theta = mb->f_ntheta[mb->nf];
		theta_iidwishart = Calloc(n_theta, double **);
		for (i = 0; i < n_theta; i++) {
			HYPER_NEW(theta_iidwishart[i], 0.0);
		}

		mb->f_theta[mb->nf] = Calloc(n_theta, double **);
		k = 0;
		for (i = 0; i < dim; i++) {
			/*
			 * first get all the precisions 
			 */
			char *init;

			if (dim == 1) {
				GMRFLib_sprintf(&init, "INITIAL");
			} else {
				GMRFLib_sprintf(&init, "INITIAL%1d", k);
			}

			tmp = iniparser_getdouble(ini, inla_string_join(secname, init), G.log_prec_initial);

			if (!mb->f_fixed[mb->nf][k] && mb->reuse_mode) {
				tmp = mb->theta_file[mb->theta_counter_file++];
			}
			SetInitial(k, tmp);
			HYPER_INIT(theta_iidwishart[k], tmp);
			if (mb->verbose) {
				printf("\t\tinitialise log_precision (component %d)[%g]\n", k + 1, tmp);
				printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][k]);
			}

			mb->f_theta[mb->nf][k] = theta_iidwishart[k];

			if (!mb->f_fixed[mb->nf][k]) {
				mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
				mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
				mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
				mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
				GMRFLib_sprintf(&msg, "Log precision for %s (component %1d)", mb->f_tag[mb->nf], k + 1);
				mb->theta_tag[mb->ntheta] = msg;
				GMRFLib_sprintf(&msg, "Precision for %s (component %1d)", mb->f_tag[mb->nf], k + 1);
				mb->theta_tag_userscale[mb->ntheta] = msg;
				GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], k);
				mb->theta_dir[mb->ntheta] = msg;

				mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
				mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
				mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].from_theta);
				mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].to_theta);

				mb->theta[mb->ntheta] = theta_iidwishart[k];
				mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
				mb->theta_map[mb->ntheta] = map_precision;
				mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
				mb->theta_map_arg[mb->ntheta] = NULL;
				mb->ntheta++;
			}

			k++;
		}
		for (i = 0; i < dim; i++) {
			for (j = i + 1; j < dim; j++) {
				/*
				 * all the correlations 
				 */
				char *init;
				GMRFLib_sprintf(&init, "INITIAL%1d", k);
				tmp = iniparser_getdouble(ini, inla_string_join(secname, init), 0.0);

				if (!mb->f_fixed[mb->nf][k] && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				}
				SetInitial(k, tmp);
				HYPER_INIT(theta_iidwishart[k], tmp);
				if (mb->verbose) {
					printf("\t\tinitialise rho_internal%1d:%1d [%g]\n", i + 1, j + 1, tmp);
					printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][k]);
				}

				mb->f_theta[mb->nf][k] = theta_iidwishart[k];

				if (!mb->f_fixed[mb->nf][k]) {
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
					GMRFLib_sprintf(&msg, "Rho_internal%1d:%1d for %s", i + 1, j + 1, mb->f_tag[mb->nf]);
					mb->theta_tag[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "Rho%1d:%1d for %s", i + 1, j + 1, mb->f_tag[mb->nf]);
					mb->theta_tag_userscale[mb->ntheta] = msg;
					GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], k);
					mb->theta_dir[mb->ntheta] = msg;

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][k].to_theta);

					mb->theta[mb->ntheta] = theta_iidwishart[k];
					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map[mb->ntheta] = map_rho;
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
					mb->theta_map_arg[mb->ntheta] = NULL;
					mb->ntheta++;
				}
				k++;
			}
		}
		assert(k == n_theta);
		break;
	}

	case F_MATERN2D:
	{
		itmp = iniparser_getint(ini, inla_string_join(secname, "NU"), 1);
		mb->f_nu[mb->nf] = itmp;
		if (mb->verbose) {
			printf("\t\tnu = [%1d]\n", itmp);
		}
		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL0"), G.log_prec_initial);
		if (!mb->f_fixed[mb->nf][0] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(0, tmp);
		HYPER_INIT(log_prec, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise log_precision[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][0]);
		}

		mb->f_theta[mb->nf] = Calloc(2, double **);
		mb->f_theta[mb->nf][0] = log_prec;
		if (!mb->f_fixed[mb->nf][0]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter0", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][0].to_theta);

			mb->theta[mb->ntheta] = log_prec;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_precision;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}

		tmp = iniparser_getdouble(ini, inla_string_join(secname, "INITIAL1"), 2.0);
		if (!mb->f_fixed[mb->nf][1] && mb->reuse_mode) {
			tmp = mb->theta_file[mb->theta_counter_file++];
		}
		SetInitial(1, tmp);
		HYPER_INIT(range_intern, tmp);
		if (mb->verbose) {
			printf("\t\tinitialise range_intern[%g]\n", tmp);
			printf("\t\tfixed=[%1d]\n", mb->f_fixed[mb->nf][1]);
		}

		mb->f_theta[mb->nf][1] = range_intern;
		if (!mb->f_fixed[mb->nf][1]) {
			/*
			 * add this \theta 
			 */
			mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
			mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
			mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
			mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
			GMRFLib_sprintf(&msg, "Range_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "Range for %s", (secname ? secname : mb->f_tag[mb->nf]));
			mb->theta_tag_userscale[mb->ntheta] = msg;
			GMRFLib_sprintf(&msg, "%s-parameter1", mb->f_dir[mb->nf]);
			mb->theta_dir[mb->ntheta] = msg;

			mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
			mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
			mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].from_theta);
			mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][1].to_theta);

			mb->theta[mb->ntheta] = range_intern;
			mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
			mb->theta_map[mb->ntheta] = map_range;
			mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
			mb->theta_map_arg[mb->ntheta] = NULL;
			mb->ntheta++;
		}
		break;
	}

	default:
		abort();
	}

	/*
	 ***
	 */

	switch (mb->f_id[mb->nf]) {
	case F_GENERIC0:
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		break;

	case F_GENERIC1:
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		break;

	case F_GENERIC2:
		assert(mb->f_N[mb->nf] == 2 * mb->f_n[mb->nf]);
		break;

	case F_GENERIC3:
		assert(mb->f_N[mb->nf] == mb->f_n[mb->nf]);
		break;

	case F_COPY:
		/*
		 * to be filled in later 
		 */
		mb->f_Qfunc[mb->nf] = NULL;
		mb->f_Qfunc_arg[mb->nf] = NULL;
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf] = -1;
		mb->f_id[mb->nf] = F_COPY;
		break;

	case F_CLINEAR:
	{
		inla_clinear_tp *def = Calloc(1, inla_clinear_tp);
		def->beta = beta;
		def->beta_arg = mb->f_theta_map_arg[mb->nf][0];
		def->precision = mb->f_precision[mb->nf];
		def->x = mb->f_locations[mb->nf];

		GMRFLib_make_linear_graph(&(mb->f_graph[mb->nf]), mb->f_n[mb->nf], 0, 0);
		mb->f_Qfunc[mb->nf] = Qfunc_clinear;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_bfunc2[mb->nf] = Calloc(1, GMRFLib_bfunc2_tp);
		mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->diagonal = mb->f_diag[mb->nf];
		mb->f_bfunc2[mb->nf]->mfunc = mfunc_clinear;
		mb->f_bfunc2[mb->nf]->mfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->n = mb->f_n[mb->nf];
		mb->f_bfunc2[mb->nf]->nreplicate = 1;
		mb->f_bfunc2[mb->nf]->ngroup = 1;
		break;
	}

	case F_SIGM:
	case F_REVSIGM:
	{
		// mb->f_id[mb->nf]
		inla_sigm_tp *def = Calloc(1, inla_sigm_tp);
		def->beta = beta;
		def->log_halflife = log_halflife;
		def->log_shape = log_shape;
		def->precision = mb->f_precision[mb->nf];
		def->x = mb->f_locations[mb->nf];

		GMRFLib_make_linear_graph(&(mb->f_graph[mb->nf]), mb->f_n[mb->nf], 0, 0);
		mb->f_Qfunc[mb->nf] = Qfunc_sigm;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_bfunc2[mb->nf] = Calloc(1, GMRFLib_bfunc2_tp);
		mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->diagonal = mb->f_diag[mb->nf];
		mb->f_bfunc2[mb->nf]->mfunc = (mb->f_id[mb->nf] == F_SIGM ? mfunc_sigm : mfunc_revsigm);
		mb->f_bfunc2[mb->nf]->mfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->n = mb->f_n[mb->nf];
		mb->f_bfunc2[mb->nf]->nreplicate = 1;
		mb->f_bfunc2[mb->nf]->ngroup = 1;
		break;
	}

	case F_BESAG:
	{
		inla_besag_Qfunc_arg_tp *arg = NULL;

		mb->f_Qfunc[mb->nf] = Qfunc_besag;
		arg = Calloc(1, inla_besag_Qfunc_arg_tp);
		GMRFLib_copy_graph(&(arg->graph), mb->f_graph[mb->nf]);
		arg->log_prec = log_prec;

		int adj = iniparser_getint(ini, inla_string_join(secname, "ADJUST.FOR.CON.COMP"), 1);
		int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);
		if (std) {
			inla_besag_scale(arg, adj);
		}
		if (mb->verbose) {
			printf("\t\tadjust.for.con.comp[%1d]\n", adj);
			printf("\t\tscale.model[%1d]\n", std);
			if (std) {
				printf("\t\tscale.model: prec_scale[%g]\n", arg->prec_scale[0]);
			}
		}

		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 1.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_BESAG;
		break;
	}

	case F_BESAG2:
	{
		inla_besag2_Qfunc_arg_tp *arg = NULL;

		mb->f_Qfunc[mb->nf] = Qfunc_besag2;
		arg = Calloc(1, inla_besag2_Qfunc_arg_tp);
		arg->graph = mb->f_graph[mb->nf];

		int adj = iniparser_getint(ini, inla_string_join(secname, "ADJUST.FOR.CON.COMP"), 1);
		int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);
		if (std) {
			inla_besag_Qfunc_arg_tp *besag_arg = Calloc(1, inla_besag_Qfunc_arg_tp);
			besag_arg->graph = arg->graph;
			inla_besag_scale(besag_arg, adj);
			arg->prec_scale = besag_arg->prec_scale;	/* yes, steal that pointer */
			Free(besag_arg);
		}
		if (mb->verbose) {
			printf("\t\tadjust.for.con.comp[%1d]\n", adj);
			printf("\t\tscale.model[%1d]\n", std);
			if (std) {
				printf("\t\tscale.model: prec_scale[%g]\n", arg->prec_scale[0]);
			}
		}

		inla_make_besag2_graph(&(mb->f_graph[mb->nf]), arg->graph);
		arg->precision = mb->f_precision[mb->nf];
		arg->log_prec = log_prec;
		arg->log_a = a_intern;

		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 1.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_BESAG2;
		break;
	}

	case F_BYM:
	{
		inla_bym_Qfunc_arg_tp *arg = NULL;
		GMRFLib_graph_tp *g = NULL;

		arg = Calloc(1, inla_bym_Qfunc_arg_tp);
		arg->besag_arg = Calloc(1, inla_besag_Qfunc_arg_tp);

		/*
		 * make the new augmented graph 
		 */
		g = mb->f_graph[mb->nf];
		inla_make_bym_graph(&(mb->f_graph[mb->nf]), g);

		/*
		 * args to the 'besag' model (spatial) 
		 */
		GMRFLib_copy_graph(&(arg->besag_arg->graph), g);
		arg->besag_arg->log_prec = log_prec1;

		int adj = iniparser_getint(ini, inla_string_join(secname, "ADJUST.FOR.CON.COMP"), 1);
		int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);
		if (std) {
			inla_besag_scale(arg->besag_arg, adj);
		}
		if (mb->verbose) {
			printf("\t\tadjust.for.con.comp[%1d]\n", adj);
			printf("\t\tscale.model[%1d]\n", std);
			if (std)
				printf("\t\tscale.model: prec_scale[%g]\n", arg->besag_arg->prec_scale[0]);
		}

		/*
		 * remaing ones 
		 */
		arg->n = mb->f_n[mb->nf];
		arg->N = mb->f_N[mb->nf] = 2 * mb->f_n[mb->nf];
		arg->log_prec_iid = log_prec0;

		/*
		 * general 
		 */
		mb->f_Qfunc[mb->nf] = Qfunc_bym;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 0.0;		       /* since constr=T is converted to extraconstr this will be corrected further below to 1 */
		break;
	}

	case F_RW2DIID:
	{
		int std;
		inla_rw2diid_Qfunc_arg_tp *arg = NULL;

		arg = Calloc(1, inla_rw2diid_Qfunc_arg_tp);
		arg->rw2ddef = Calloc(1, GMRFLib_rw2ddef_tp);
		arg->rw2ddef->nrow = mb->f_nrow[mb->nf];
		arg->rw2ddef->ncol = mb->f_ncol[mb->nf];
		arg->rw2ddef->order = 2;
		arg->rw2ddef->bvalue = bvalue;
		arg->rw2ddef->cyclic = mb->f_cyclic[mb->nf];

		inla_make_rw2diid_graph(&(mb->f_graph[mb->nf]), arg->rw2ddef);
		std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 1);
		assert(std == 1);			       /* this has to be true for this model */
		GMRFLib_rw2d_scale(arg->rw2ddef);
		if (mb->verbose) {
			printf("\t\tscale.model[%1d]\n", std);
			printf("\t\tscale.model: prec_scale[%g]\n", arg->rw2ddef->prec_scale[0]);
		}

		/*
		 * remaing ones 
		 */
		arg->n = mb->f_n[mb->nf];
		arg->N = mb->f_N[mb->nf] = 2 * mb->f_n[mb->nf];
		arg->log_prec = log_prec;
		arg->logit_phi = phi_intern;

		/*
		 * general 
		 */
		mb->f_Qfunc[mb->nf] = Qfunc_rw2diid;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 0.0;		       /* since constr=T is converted to extraconstr this will be corrected further below to 1 */
		break;
	}

	case F_BYM2:
	{
		inla_bym2_Qfunc_arg_tp *arg = NULL;
		GMRFLib_graph_tp *g = NULL;

		arg = Calloc(1, inla_bym2_Qfunc_arg_tp);
		arg->besag_arg = Calloc(1, inla_besag_Qfunc_arg_tp);

		/*
		 * make the new augmented graph 
		 */
		g = mb->f_graph[mb->nf];
		inla_make_bym_graph(&(mb->f_graph[mb->nf]), g);	/* yes, its the same graph */
		GMRFLib_copy_graph(&(arg->besag_arg->graph), g);

		int adj = iniparser_getint(ini, inla_string_join(secname, "ADJUST.FOR.CON.COMP"), 1);
		int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 1);
		assert(std == 1);			       /* this has to be true for this model */

		inla_besag_scale(arg->besag_arg, adj);
		if (mb->verbose) {
			printf("\t\tadjust.for.con.comp[%1d]\n", adj);
			printf("\t\tscale.model[%1d]\n", std);
			printf("\t\tscale.model: prec_scale[%g]\n", arg->besag_arg->prec_scale[0]);
		}

		/*
		 * remaing ones 
		 */
		arg->n = mb->f_n[mb->nf];
		arg->N = mb->f_N[mb->nf] = 2 * mb->f_n[mb->nf];
		arg->log_prec = log_prec0;
		arg->logit_phi = phi_intern;

		/*
		 * general 
		 */
		mb->f_Qfunc[mb->nf] = Qfunc_bym2;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 0.0;		       /* since constr=T is converted to extraconstr this will be corrected further below to 1 */
		break;
	}

	case F_BESAGPROPER:
	{
		inla_besag_proper_Qfunc_arg_tp *arg = NULL, *arg_orig = NULL;
		arg = Calloc(1, inla_besag_proper_Qfunc_arg_tp);
		arg_orig = Calloc(1, inla_besag_proper_Qfunc_arg_tp);

		mb->f_Qfunc[mb->nf] = Qfunc_besagproper;
		mb->f_Qfunc_orig[mb->nf] = Qfunc_besagproper;
		GMRFLib_copy_graph(&arg->graph, mb->f_graph[mb->nf]);
		GMRFLib_copy_graph(&arg_orig->graph, mb->f_graph[mb->nf]);
		GMRFLib_copy_graph(&mb->f_graph_orig[mb->nf], mb->f_graph[mb->nf]);
		arg->log_prec = log_prec;
		arg->log_diag = log_diag;
		arg_orig->log_prec = arg_orig->log_diag = NULL;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_BESAGPROPER;
		break;
	}

	case F_BESAGPROPER2:
	{
		inla_besag_proper2_Qfunc_arg_tp *arg = NULL, *arg_orig = NULL;
		arg = Calloc(1, inla_besag_proper2_Qfunc_arg_tp);
		arg_orig = Calloc(1, inla_besag_proper2_Qfunc_arg_tp);

		mb->f_Qfunc[mb->nf] = Qfunc_besagproper2;
		mb->f_Qfunc_orig[mb->nf] = Qfunc_besagproper2;
		GMRFLib_copy_graph(&arg->graph, mb->f_graph[mb->nf]);
		GMRFLib_copy_graph(&arg_orig->graph, mb->f_graph[mb->nf]);
		GMRFLib_copy_graph(&mb->f_graph_orig[mb->nf], mb->f_graph[mb->nf]);
		arg->log_prec = log_prec;
		arg->logit_lambda = phi_intern;
		arg_orig->log_prec = arg_orig->logit_lambda = NULL;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_BESAGPROPER;
		break;
	}

	case F_SPDE:
		mb->f_Qfunc[mb->nf] = spde_model->Qfunc;
		mb->f_Qfunc_arg[mb->nf] = spde_model->Qfunc_arg;
		mb->f_graph[mb->nf] = spde_model->graph;
		mb->f_rankdef[mb->nf] = 0;
		mb->f_n[mb->nf] = mb->f_N[mb->nf] = spde_model->n;
		break;

	case F_SPDE2:
		mb->f_Qfunc[mb->nf] = spde2_model->Qfunc;
		mb->f_Qfunc_arg[mb->nf] = spde2_model->Qfunc_arg;
		mb->f_graph[mb->nf] = spde2_model->graph;
		mb->f_rankdef[mb->nf] = 0;
		mb->f_n[mb->nf] = mb->f_N[mb->nf] = spde2_model->n;
		break;

	case F_SPDE3:
		mb->f_Qfunc[mb->nf] = spde3_model->Qfunc;
		mb->f_Qfunc_arg[mb->nf] = spde3_model->Qfunc_arg;
		mb->f_graph[mb->nf] = spde3_model->graph;
		mb->f_rankdef[mb->nf] = 0;
		mb->f_n[mb->nf] = mb->f_N[mb->nf] = spde3_model->n;
		break;

	case F_AR:
	{
		GMRFLib_graph_tp *g;
		ar_def_tp *def = Calloc(1, ar_def_tp);

		def->n = mb->f_n[mb->nf];
		def->p = mb->f_order[mb->nf];
		def->log_prec = log_prec;
		def->pacf_intern = pacf_intern;

		def->hold_pacf_intern = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
		def->hold_Q = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
		def->hold_Qmarg = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
		for (i = 0; i < ISQR(GMRFLib_MAX_THREADS); i++) {
			def->hold_pacf_intern[i] = Calloc(def->p, double);
			for (j = 0; j < def->p; j++) {
				def->hold_pacf_intern[i][j] = GMRFLib_uniform();
			}
		}

		GMRFLib_make_linear_graph(&g, def->n, def->p, 0);
		mb->f_Qfunc[mb->nf] = Qfunc_ar;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_graph[mb->nf] = g;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		break;
	}

	case F_RW2D:
	{
		GMRFLib_rw2ddef_tp *arg = NULL;

		mb->f_Qfunc[mb->nf] = GMRFLib_rw2d;
		arg = Calloc(1, GMRFLib_rw2ddef_tp);
		arg->nrow = mb->f_nrow[mb->nf];
		arg->ncol = mb->f_ncol[mb->nf];
		arg->order = 2;
		arg->cyclic = mb->f_cyclic[mb->nf];
		arg->bvalue = bvalue;
		arg->prec = NULL;
		arg->log_prec = NULL;
		arg->log_prec_omp = log_prec;

		int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);
		if (std) {
			GMRFLib_rw2d_scale((void *) arg);
		}
		if (mb->verbose) {
			printf("\t\tscale.model[%1d]\n", std);
			if (std)
				printf("\t\tscale.model: prec_scale[%g]\n", arg->prec_scale[0]);
		}

		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = (bvalue == GMRFLib_BVALUE_ZERO ? 0.0 : (arg->cyclic ? 1.0 : 3.0));
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_RW2D;
		GMRFLib_make_rw2d_graph(&(mb->f_graph[mb->nf]), arg);
		break;
	}

	case F_Z:
	{
		char *Am = NULL, *Bm = NULL;
		GMRFLib_tabulate_Qfunc_tp *Qfunc_A = NULL, *Qfunc_B = NULL;
		GMRFLib_graph_tp *graph_A = NULL, *graph_B = NULL, *graph_AB = NULL, *gs[2];
		inla_z_arg_tp *arg = NULL, *arg_orig = NULL;
		double **log_prec_orig = NULL;

		Am = iniparser_getstring(ini, inla_string_join(secname, "z.Amatrix"), NULL);
		Bm = iniparser_getstring(ini, inla_string_join(secname, "z.Bmatrix"), NULL);
		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_A, &graph_A, Am, zn + zm, NULL, NULL, NULL);
		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_B, &graph_B, Bm, zn + zm, NULL, NULL, NULL);

		gs[0] = graph_A;
		gs[1] = graph_B;
		GMRFLib_union_graph(&graph_AB, gs, 2);

		arg = Calloc(1, inla_z_arg_tp);
		arg->n = zn;
		arg->m = zm;
		arg->log_prec = log_prec;
		arg->graph_A = graph_A;
		arg->Qfunc_A = Qfunc_A;
		arg->graph_B = graph_B;
		arg->Qfunc_B = Qfunc_B;
		arg->graph_AB = graph_AB;

		HYPER_NEW(log_prec_orig, log_prec[0][0]);
		arg_orig = Calloc(1, inla_z_arg_tp);
		memcpy(arg_orig, arg, sizeof(inla_z_arg_tp));
		arg_orig->log_prec = log_prec_orig;

		mb->f_Qfunc[mb->nf] = Qfunc_z;
		mb->f_Qfunc_orig[mb->nf] = Qfunc_z;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
		mb->f_rankdef[mb->nf] = 0;		       /* default value */
		GMRFLib_copy_graph(&(mb->f_graph[mb->nf]), graph_AB);
		GMRFLib_copy_graph(&(mb->f_graph_orig[mb->nf]), graph_AB);
		break;
	}

	case F_SLM:
	{
		char *A1m = NULL, *A2m = NULL, *Bm = NULL, *Cm = NULL;
		GMRFLib_tabulate_Qfunc_tp *Qfunc_A1 = NULL, *Qfunc_A2 = NULL, *Qfunc_B = NULL, *Qfunc_C = NULL;
		GMRFLib_graph_tp *graph_A1 = NULL, *graph_A2 = NULL, *graph_B = NULL, *graph_C = NULL, *graph_slm = NULL, *gs[4];
		inla_slm_arg_tp *arg = NULL, *arg_orig = NULL;
		double **log_prec_orig = NULL, **logit_rho_orig = NULL;

		A1m = iniparser_getstring(ini, inla_string_join(secname, "slm.A1matrix"), NULL);
		A2m = iniparser_getstring(ini, inla_string_join(secname, "slm.A2matrix"), NULL);
		Bm = iniparser_getstring(ini, inla_string_join(secname, "slm.Bmatrix"), NULL);
		Cm = iniparser_getstring(ini, inla_string_join(secname, "slm.Cmatrix"), NULL);

		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_A1, &graph_A1, A1m, slm_n + slm_m, NULL, NULL, NULL);
		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_A2, &graph_A2, A2m, slm_n + slm_m, NULL, NULL, NULL);
		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_B, &graph_B, Bm, slm_n + slm_m, NULL, NULL, NULL);
		GMRFLib_tabulate_Qfunc_from_file(&Qfunc_C, &graph_C, Cm, slm_n + slm_m, NULL, NULL, NULL);

		// FIXME("Matrix A1");
		// GMRFLib_print_Qfunc(stdout, graph_A1, Qfunc_A1->Qfunc, Qfunc_A1->Qfunc_arg);
		// exit(0);

		gs[0] = graph_A1;
		gs[1] = graph_A2;
		gs[2] = graph_B;
		gs[3] = graph_C;
		GMRFLib_union_graph(&graph_slm, gs, 4);

		arg = Calloc(1, inla_slm_arg_tp);
		arg->rho_min = slm_rho_min;
		arg->rho_max = slm_rho_max;
		arg->n = slm_n;
		arg->m = slm_m;
		arg->log_prec = log_prec;
		arg->logit_rho = rho_intern;

		arg->graph_A1 = graph_A1;
		arg->graph_A2 = graph_A2;
		arg->graph_B = graph_B;
		arg->graph_C = graph_C;
		arg->graph_slm = graph_slm;

		arg->Qfunc_A1 = Qfunc_A1;
		arg->Qfunc_A2 = Qfunc_A2;
		arg->Qfunc_B = Qfunc_B;
		arg->Qfunc_C = Qfunc_C;

		HYPER_NEW(log_prec_orig, log_prec[0][0]);
		HYPER_NEW(logit_rho_orig, rho_intern[0][0]);
		arg_orig = Calloc(1, inla_slm_arg_tp);
		memcpy(arg_orig, arg, sizeof(inla_slm_arg_tp));
		arg_orig->log_prec = log_prec_orig;
		arg_orig->logit_rho = logit_rho_orig;

		mb->f_Qfunc[mb->nf] = Qfunc_slm;
		mb->f_Qfunc_orig[mb->nf] = Qfunc_slm;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
		mb->f_rankdef[mb->nf] = 0;		       /* default value */
		GMRFLib_copy_graph(&(mb->f_graph[mb->nf]), graph_slm);
		GMRFLib_copy_graph(&(mb->f_graph_orig[mb->nf]), graph_slm);
		break;
	}

	case F_2DIID:
	{
		inla_2diid_arg_tp *arg = NULL;

		mb->f_N[mb->nf] = 2 * mb->f_n[mb->nf];
		arg = Calloc(1, inla_2diid_arg_tp);
		arg->n = mb->f_n[mb->nf];
		arg->log_prec0 = log_prec0;
		arg->log_prec1 = log_prec1;
		arg->rho_intern = rho_intern;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		if (mb->f_id[mb->nf] == F_2DIID) {
			mb->f_Qfunc[mb->nf] = Qfunc_2diid;
			inla_make_2diid_graph(&(mb->f_graph[mb->nf]), arg);
		} else {
			mb->f_Qfunc[mb->nf] = Qfunc_2diid_wishart;
			inla_make_2diid_wishart_graph(&(mb->f_graph[mb->nf]), arg);
		}
		mb->f_rankdef[mb->nf] = 0;
		break;
	}

	case F_IID1D:
	case F_IID2D:
	case F_IID3D:
	case F_IID4D:
	case F_IID5D:
	{
		inla_iid_wishart_arg_tp *arg = NULL;
		int dim = WISHART_DIM(mb->nf);
		assert(dim > 0);

		assert(mb->f_N[mb->nf] == mb->f_n[mb->nf]);
		arg = Calloc(1, inla_iid_wishart_arg_tp);
		arg->dim = dim;
		arg->n = mb->f_n[mb->nf] / dim;		       /* yes */
		arg->N = mb->f_N[mb->nf];
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_rankdef[mb->nf] = 0;
		arg->log_prec = theta_iidwishart;
		arg->rho_intern = theta_iidwishart + dim;
		arg->hold = Calloc(ISQR(GMRFLib_MAX_THREADS), inla_wishart_hold_tp *);
		mb->f_Qfunc[mb->nf] = Qfunc_iid_wishart;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		inla_make_iid_wishart_graph(&(mb->f_graph[mb->nf]), arg);
		break;
	}

	case F_SEASONAL:
	{
		GMRFLib_seasonaldef_tp *sdef = NULL;

		mb->f_Qfunc[mb->nf] = GMRFLib_seasonal;
		sdef = Calloc(1, GMRFLib_seasonaldef_tp);
		sdef->n = n;
		sdef->s = s;
		sdef->cyclic = mb->f_cyclic[mb->nf];
		sdef->prec = NULL;
		sdef->log_prec = NULL;
		sdef->log_prec_omp = log_prec;
		GMRFLib_make_seasonal_graph(&(mb->f_graph[mb->nf]), sdef);
		mb->f_Qfunc_arg[mb->nf] = (void *) sdef;
		/*
		 * for the rank-deficieny, we know the result for CYCLIC=FALSE, but we need to compute it for CYCLIC=TRUE 
		 */
		if (!mb->f_cyclic[mb->nf]) {
			mb->f_rankdef[mb->nf] = s - 1.0;
		} else {
			double *chol = NULL, eps = 1.0e-8, *Q = NULL;
			int *map = NULL, rank;
			GMRFLib_graph_tp *g = NULL;

			g = mb->f_graph[mb->nf];
			Q = Calloc(ISQR(n), double);

			for (i = 0; i < n; i++) {
				Q[i + i * n] = GMRFLib_seasonal(i, i, (void *) sdef);
				for (jj = 0; jj < g->nnbs[i]; jj++) {
					j = g->nbs[i][jj];
					Q[j + i * n] = Q[i + j * n] = GMRFLib_seasonal(i, j, (void *) sdef);
				}
			}
			GMRFLib_comp_chol_semidef(&chol, &map, &rank, Q, n, NULL, eps);
			if (mb->verbose) {
				printf("\t\tcomputed default rank deficiency [%1d]\n", n - rank);
			}
			mb->f_rankdef[mb->nf] = n - rank;
			Free(Q);
			Free(chol);
			Free(map);
		}
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_SEASONAL;
		break;
	}

	case F_MEC:
	{
		/*
		 * MEC
		 */
		char *filename_s;
		filename_s = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SCALE"), NULL));
		if (filename_s) {
			if (mb->verbose) {
				printf("\t\tread scale from file=[%s]\n", filename_s);
			}
			inla_read_data_general(&(mb->f_scale[mb->nf]), NULL, NULL, filename_s, mb->predictor_n, 0, 1, mb->verbose, 1.0);
		} else {
			if (mb->verbose) {
				printf("\t\tno scale\n");
			}
			mb->f_scale[mb->nf] = Calloc(mb->predictor_n, double);
			int ii;
			for (ii = 0; ii < mb->predictor_n; ii++) {
				mb->f_scale[mb->nf][ii] = 1.0;
			}
		}

		inla_mec_tp *def = Calloc(1, inla_mec_tp);

		def->beta = beta;
		def->log_prec_obs = log_prec;
		def->mean_x = mean_x;
		def->log_prec_x = log_prec_x;
		def->x_obs = mb->f_locations[mb->nf];
		// must make a copy... (realloc)
		def->scale = Calloc(mb->predictor_n, double);
		def->map_beta_arg = mb->f_theta_map_arg[mb->nf][0];
		memcpy(def->scale, mb->f_scale[mb->nf], mb->predictor_n * sizeof(double));

		GMRFLib_make_linear_graph(&(mb->f_graph[mb->nf]), mb->f_n[mb->nf], 0, 0);
		mb->f_Qfunc[mb->nf] = Qfunc_mec;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;

		mb->f_bfunc2[mb->nf] = Calloc(1, GMRFLib_bfunc2_tp);
		mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->diagonal = mb->f_diag[mb->nf];
		mb->f_bfunc2[mb->nf]->mfunc = mfunc_mec;
		mb->f_bfunc2[mb->nf]->mfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->n = mb->f_n[mb->nf];
		mb->f_bfunc2[mb->nf]->nreplicate = 1;
		mb->f_bfunc2[mb->nf]->ngroup = 1;
		break;
	}

	case F_MEB:
	{
		/*
		 * MEB
		 */
		char *filename_s;
		filename_s = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SCALE"), NULL));
		if (filename_s) {
			if (mb->verbose) {
				printf("\t\tread scale from file=[%s]\n", filename_s);
			}
			inla_read_data_general(&(mb->f_scale[mb->nf]), NULL, NULL, filename_s, mb->predictor_n, 0, 1, mb->verbose, 1.0);
		} else {
			if (mb->verbose) {
				printf("\t\tno scale\n");
			}
			mb->f_scale[mb->nf] = Calloc(mb->predictor_n, double);
			int ii;
			for (ii = 0; ii < mb->predictor_n; ii++) {
				mb->f_scale[mb->nf][ii] = 1.0;
			}
		}

		inla_meb_tp *def = Calloc(1, inla_meb_tp);

		def->beta = beta;
		def->log_prec = log_prec;
		def->x = mb->f_locations[mb->nf];
		// must make a copy... (realloc)
		def->scale = Calloc(mb->predictor_n, double);
		memcpy(def->scale, mb->f_scale[mb->nf], mb->predictor_n * sizeof(double));
		def->map_beta_arg = mb->f_theta_map_arg[mb->nf][0];

		GMRFLib_make_linear_graph(&(mb->f_graph[mb->nf]), mb->f_n[mb->nf], 0, 0);
		mb->f_Qfunc[mb->nf] = Qfunc_meb;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;

		mb->f_bfunc2[mb->nf] = Calloc(1, GMRFLib_bfunc2_tp);
		mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
		mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->diagonal = mb->f_diag[mb->nf];
		mb->f_bfunc2[mb->nf]->mfunc = mfunc_meb;
		mb->f_bfunc2[mb->nf]->mfunc_arg = mb->f_Qfunc_arg[mb->nf];
		mb->f_bfunc2[mb->nf]->n = mb->f_n[mb->nf];
		mb->f_bfunc2[mb->nf]->nreplicate = 1;
		mb->f_bfunc2[mb->nf]->ngroup = 1;
		break;
	}

	case F_R_GENERIC:
	{
		/*
		 * R_GENERIC
		 */
#if !defined(WINDOWS)
		inla_rgeneric_tp *def = Calloc(1, inla_rgeneric_tp);

		def->Id = Id;
		def->R2c = R2c;
		def->c2R = c2R;
		def->filename_R2c = GMRFLib_strdup(filename_R2c);
		def->filename_c2R = GMRFLib_strdup(filename_c2R);
		def->ntheta = mb->f_ntheta[mb->nf];
		def->theta = mb->f_theta[mb->nf];
		def->param = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);	/* easier if we do this here */
		def->Q = Calloc(ISQR(GMRFLib_MAX_THREADS), GMRFLib_tabulate_Qfunc_tp *);	/* easier if we do this here */

		int len, *ilist, *jlist;
#pragma omp critical
		{
			WRITE_MSG(c2R, def->Id, R_GENERIC_GRAPH);
			READ(R2c, &len, 1, int);
			ilist = Calloc(len, int);
			jlist = Calloc(len, int);
			READ(R2c, ilist, len, int);
			READ(R2c, jlist, len, int);
		}

		double *Qijlist = Calloc(len, double);
		for (i = 0; i < len; i++) {
			Qijlist[i] = 1.0;
		}

		GMRFLib_tabulate_Qfunc_tp *tab;
		GMRFLib_graph_tp *graph;

		GMRFLib_tabulate_Qfunc_from_list(&tab, &graph, len, ilist, jlist, Qijlist, -1, NULL, NULL, NULL);
		GMRFLib_free_tabulate_Qfunc(tab);
		Free(ilist);
		Free(jlist);
		Free(Qijlist);

		mb->f_graph[mb->nf] = graph;
		mb->f_Qfunc[mb->nf] = Qfunc_rgeneric;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf] = graph->n;
		mb->f_rankdef[mb->nf] = 0.0;
#endif							       /* !defined(WINDOWS) */
		break;
	}

	case F_AR1:
	{
		/*
		 * AR1 
		 */
		inla_ar1_arg_tp *def = NULL;

		def = Calloc(1, inla_ar1_arg_tp);
		def->n = mb->f_n[mb->nf];
		def->cyclic = mb->f_cyclic[mb->nf];
		def->log_prec = log_prec;
		def->phi_intern = phi_intern;
		inla_make_ar1_graph(&(mb->f_graph[mb->nf]), def);
		mb->f_Qfunc[mb->nf] = Qfunc_ar1;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;
		break;
	}

	case F_OU:
	{
		/*
		 * OU
		 */
		inla_ou_arg_tp *def = NULL;

		def = Calloc(1, inla_ou_arg_tp);
		def->n = mb->f_n[mb->nf];
		def->log_prec = log_prec;
		def->phi_intern = phi_intern;
		assert(mb->f_locations[mb->nf]);
		def->locations = mb->f_locations[mb->nf];
		inla_make_ou_graph(&(mb->f_graph[mb->nf]), def);
		mb->f_Qfunc[mb->nf] = Qfunc_ou;
		mb->f_Qfunc_arg[mb->nf] = (void *) def;

		/*
		 * need this one later to get 'n'. a copy of the original contents is ok. 
		 */
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) Calloc(1, inla_ou_arg_tp);
		memcpy(mb->f_Qfunc_arg_orig[mb->nf], mb->f_Qfunc_arg[mb->nf], sizeof(inla_ou_arg_tp));

		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_rankdef[mb->nf] = 0.0;
		break;
	}

	case F_MATERN2D:
	{
		/*
		 * MATERN2D
		 */
		GMRFLib_matern2ddef_tp *arg = NULL;

		arg = Calloc(1, GMRFLib_matern2ddef_tp);
		arg->nrow = mb->f_nrow[mb->nf];
		arg->ncol = mb->f_ncol[mb->nf];
		arg->cyclic = mb->f_cyclic[mb->nf];
		arg->nu = mb->f_nu[mb->nf];
		arg->prec = NULL;
		arg->log_prec = NULL;
		arg->log_prec_omp = log_prec;
		arg->range = NULL;
		arg->log_range = NULL;
		arg->log_range_omp = range_intern;

		GMRFLib_matern2ddef_tp *arg_orig = NULL;
		arg_orig = Calloc(1, GMRFLib_matern2ddef_tp);
		memcpy(arg_orig, arg, sizeof(GMRFLib_matern2ddef_tp));

		mb->f_Qfunc[mb->nf] = GMRFLib_matern2d;
		mb->f_Qfunc_orig[mb->nf] = GMRFLib_matern2d;
		mb->f_Qfunc_arg[mb->nf] = (void *) arg;
		mb->f_Qfunc_arg_orig[mb->nf] = (void *) arg_orig;
		mb->f_rankdef[mb->nf] = 0.0;
		mb->f_N[mb->nf] = mb->f_n[mb->nf];
		mb->f_id[mb->nf] = F_MATERN2D;
		GMRFLib_make_matern2d_graph(&(mb->f_graph[mb->nf]), arg);
		GMRFLib_make_matern2d_graph(&(mb->f_graph_orig[mb->nf]), arg);
		break;
	}

	default:
	{
		/*
		 * RW-models. do a special test for cyclic, since this require locations = default
		 */
		if ((mb->f_id[mb->nf] == F_IID || mb->f_id[mb->nf] == F_RW1 || mb->f_id[mb->nf] == F_RW2) && mb->f_cyclic[mb->nf]) {
			GMRFLib_rwdef_tp *rwdef = NULL;

			if (mb->f_locations[mb->nf]) {
				printf("\n*** Warning ***\tModel[%s] in Section[%s] has cyclic = TRUE but values != NULL.\n", model, secname);
				printf("*** Warning ***\tCylic = TRUE is not implemented for non-equal spaced values.\n");
				printf("*** Warning ***\tAssume values are equal spaced.\n\n");
			}

			int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);

			rwdef = Calloc(1, GMRFLib_rwdef_tp);
			rwdef->n = mb->f_n[mb->nf];
			if (mb->f_id[mb->nf] == F_IID) {
				rwdef->order = 0;

				/*
				 * this case has an extra option: scale
				 */
				char *filename_s;
				filename_s = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SCALE"), NULL));
				if (filename_s) {
					if (mb->verbose) {
						printf("\t\tread scale from file=[%s]\n", filename_s);
					}
					inla_read_data_general(&(rwdef->scale0), NULL, NULL, filename_s, rwdef->n, 0, 1, mb->verbose, 1.0);
					mb->f_scale[mb->nf] = rwdef->scale0;	/* need a copy */
				} else {
					rwdef->scale0 = NULL;
				}
			} else if (mb->f_id[mb->nf] == F_RW1) {
				rwdef->order = 1;
			} else {
				assert(mb->f_id[mb->nf] == F_RW2);
				rwdef->order = 2;
			}
			assert(rwdef->n > rwdef->order);
			rwdef->prec = NULL;
			rwdef->log_prec = NULL;
			rwdef->log_prec_omp = log_prec;
			rwdef->cyclic = mb->f_cyclic[mb->nf];
			if (mb->f_cyclic[mb->nf]) {
				if (rwdef->order == 0) {
					mb->f_rankdef[mb->nf] = 0.0;
				} else {
					mb->f_rankdef[mb->nf] = 1.0;
				}
			} else {
				abort();
			}
			GMRFLib_make_rw_graph(&(mb->f_graph[mb->nf]), rwdef);
			if (std) {
				GMRFLib_rw_scale((void *) rwdef);
			}
			if (mb->verbose) {
				printf("\t\tscale.model[%1d]\n", std);
				if (std)
					printf("\t\tscale.model: prec_scale[%g]\n", rwdef->prec_scale[0]);
			}

			mb->f_Qfunc[mb->nf] = GMRFLib_rw;
			mb->f_Qfunc_arg[mb->nf] = (void *) rwdef;
			mb->f_N[mb->nf] = mb->f_graph[mb->nf]->n;
		} else if ((mb->f_id[mb->nf] == F_IID || mb->f_id[mb->nf] == F_RW1 ||
			    mb->f_id[mb->nf] == F_RW2 || mb->f_id[mb->nf] == F_CRW2) && !mb->f_cyclic[mb->nf]) {
			crwdef = Calloc(1, GMRFLib_crwdef_tp);
			crwdef->n = mb->f_n[mb->nf];
			crwdef->prec = NULL;
			crwdef->log_prec = NULL;
			crwdef->log_prec_omp = log_prec;
			if (mb->f_id[mb->nf] == F_IID) {
				crwdef->order = 0;
				/*
				 * this case has an extra option: scale
				 */
				char *filename_s;
				filename_s = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "SCALE"), NULL));
				if (filename_s) {
					if (mb->verbose) {
						printf("\t\tread scale from file=[%s]\n", filename_s);
					}
					inla_read_data_general(&(crwdef->scale0), NULL, NULL, filename_s, crwdef->n, 0, 1, mb->verbose, 1.0);
					mb->f_scale[mb->nf] = crwdef->scale0;	/* need a copy */
				} else {
					crwdef->scale0 = NULL;
				}
				crwdef->layout = GMRFLib_CRW_LAYOUT_SIMPLE;
				mb->f_rankdef[mb->nf] = 0.0;
			} else if (mb->f_id[mb->nf] == F_RW1) {
				crwdef->order = 1;
				crwdef->layout = GMRFLib_CRW_LAYOUT_SIMPLE;
				mb->f_rankdef[mb->nf] = 1.0;
			} else if (mb->f_id[mb->nf] == F_RW2) {
				crwdef->order = 2;
				crwdef->layout = GMRFLib_CRW_LAYOUT_SIMPLE;
				mb->f_rankdef[mb->nf] = 2.0;
			} else if (mb->f_id[mb->nf] == F_CRW2) {
				crwdef->order = 2;
				crwdef->layout = GMRFLib_CRW_LAYOUT_BLOCK;
				mb->f_rankdef[mb->nf] = 2.0;

				/*
				 * duplicate the locations and swap the sign, if they are present
				 */
				if (mb->f_locations[mb->nf]) {
					double *t = Calloc(2 * mb->f_n[mb->nf], double);
					memcpy(&t[0], mb->f_locations[mb->nf], mb->f_n[mb->nf] * sizeof(double));
					memcpy(&t[mb->f_n[mb->nf]], mb->f_locations[mb->nf], mb->f_n[mb->nf] * sizeof(double));

					int ii;
					for (ii = mb->f_n[mb->nf]; ii < 2 * mb->f_n[mb->nf]; ii++) {
						t[ii] *= -1.0;
					}

					mb->f_locations[mb->nf] = t;
				}
			} else {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "model", model);
			}
			crwdef->position = mb->f_locations[mb->nf];	/* do this here, as the locations are duplicated for CRW2 */
			assert(crwdef->n > crwdef->order);
			int std = iniparser_getint(ini, inla_string_join(secname, "SCALE.MODEL"), 0);
			if (mb->f_id[mb->nf] == F_RW1 || mb->f_id[mb->nf] == F_RW2 || mb->f_id[mb->nf] == F_CRW2) {
				if (std) {
					GMRFLib_crw_scale((void *) crwdef);
				}
				if (mb->verbose) {
					printf("\t\tscale.model[%1d]\n", std);
					if (std)
						printf("\t\tscale.model: prec_scale[%g]\n", crwdef->prec_scale[0]);
				}
			} else {
				if (std) {
					char *msg;
					GMRFLib_sprintf(&msg, "model[%s]. scale.model=TRUE but this model cannot be scaled. Contact developers\n", model);
					inla_error_general(msg);
					exit(1);
				}
			}

			GMRFLib_make_crw_graph(&(mb->f_graph[mb->nf]), crwdef);
			mb->f_Qfunc[mb->nf] = GMRFLib_crw;
			mb->f_Qfunc_arg[mb->nf] = (void *) crwdef;
			mb->f_N[mb->nf] = mb->f_graph[mb->nf]->n;
		} else {
			assert(0 == 1);
		}
	}
	}

	/*
	 * read optional extra constraint 
	 */
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "EXTRACONSTRAINT"), NULL));
	if (filename) {
		if (mb->verbose) {
			printf("\t\tread extra constraint from file=[%s]\n", filename);
		}
		mb->f_constr[mb->nf] = inla_read_constraint(filename, mb->f_N[mb->nf]);
		if (mb->verbose) {
			int nnc = mb->f_constr[mb->nf]->nc;

			for (j = 0; j < nnc; j++) {
				printf("\t\tConstraint[%1d]\n", j);
				int k = 0;
				for (i = 0; i < mb->f_N[mb->nf]; i++) {
					double a = mb->f_constr[mb->nf]->a_matrix[i * nnc + j];
					if (!ISZERO(a) || mb->f_N[mb->nf] <= PREVIEW) {
						printf("\t\t\tA[%1d] = %f\n", i, a);
						k++;
					}
					if (k > PREVIEW)
						break;
				}
				printf("\t\t\te[%1d] = %f\n", j, mb->f_constr[mb->nf]->e_vector[j]);
			}
		}
	}

	/*
	 * hold a copy of the original constraints before `group' and `replicate' 
	 */
	mb->f_constr_orig[mb->nf] = inla_make_constraint(mb->f_N[mb->nf], mb->f_sumzero[mb->nf], mb->f_constr[mb->nf]);

	/*
	 * determine the final rankdef. 
	 */
	rd = iniparser_getdouble(ini, inla_string_join(secname, "RANKDEF"), -1.0);
	if (rd >= 0) {
		/*
		 * if RANKDEF is given, then this is used, not matter what! 
		 */
		mb->f_rankdef[mb->nf] = rd;
		if (mb->verbose) {
			printf("\t\trank-deficiency is *defined* [%g]\n", rd);
		}
	} else {
		/*
		 * use the previously set default value for the rankdef.  only in the case of a proper model, correct for sumzero
		 * constraint 
		 */
		if (ISZERO(mb->f_rankdef[mb->nf])) {
			mb->f_rankdef[mb->nf] = (mb->f_sumzero[mb->nf] ? 1.0 : 0.0);
		}
		/*
		 * if extra constraint(s), then correct for this. OOPS: this *can* be wrong, if the extra constraint are in the
		 * NULL-space of Q, but then the RANKDEF *is* required. 
		 */
		mb->f_rankdef[mb->nf] += (mb->f_constr[mb->nf] ? mb->f_constr[mb->nf]->nc : 0.0);
		if (mb->verbose) {
			printf("\t\tcomputed/guessed rank-deficiency = [%g]\n", mb->f_rankdef[mb->nf]);
		}
	}
	inla_parse_output(mb, ini, sec, &(mb->f_output[mb->nf]));

	/*
	 * for all models except the F_COPY one, do the group and replicate expansions 
	 */
	if (mb->f_id[mb->nf] != F_COPY) {

		if (mb->f_ngroup[mb->nf] > 1) {
			/*
			 * add groups! 
			 */
			ptmp = GMRFLib_strdup("EXCHANGEABLE");
			ptmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GROUP.MODEL"), ptmp));
			if (!strcasecmp(ptmp, "EXCHANGEABLE")) {
				mb->f_group_model[mb->nf] = G_EXCHANGEABLE;
			} else if (!strcasecmp(ptmp, "AR1")) {
				mb->f_group_model[mb->nf] = G_AR1;
			} else if (!strcasecmp(ptmp, "AR")) {
				mb->f_group_model[mb->nf] = G_AR;
			} else if (!strcasecmp(ptmp, "RW1")) {
				mb->f_group_model[mb->nf] = G_RW1;
			} else if (!strcasecmp(ptmp, "RW2")) {
				mb->f_group_model[mb->nf] = G_RW2;
			} else if (!strcasecmp(ptmp, "BESAG")) {
				mb->f_group_model[mb->nf] = G_BESAG;
			} else {
				GMRFLib_sprintf(&msg, "%s: Unknown GROUP.MODEL: %s\n", secname, ptmp);
				inla_error_general(msg);
				abort();
			}

			ptmp2 = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "GROUP.GRAPH"), NULL));
			if (ptmp2) {
				GMRFLib_read_graph(&(mb->f_group_graph[mb->nf]), ptmp2);
			}

			mb->f_group_cyclic[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "GROUP.CYCLIC"), 0);
			mb->f_group_order[mb->nf] = iniparser_getint(ini, inla_string_join(secname, "GROUP.ORDER"), -1);	/* will force an error if not set
																 * and used */
			if (mb->verbose) {
				printf("\t\tgroup.model = %s\n", ptmp);
				printf("\t\tgroup.graph = %s\n", (ptmp2 ? ptmp2 : "<NONE>"));
				printf("\t\tgroup.cyclic = %s\n", (mb->f_group_cyclic[mb->nf] ? "True" : "False"));
				printf("\t\tgroup.order = %1d\n", mb->f_group_order[mb->nf]);
			}

			switch (mb->f_group_model[mb->nf]) {
			case G_EXCHANGEABLE:
			case G_AR1:
			case G_RW1:
			case G_RW2:
			case G_BESAG:
				fixed = iniparser_getboolean(ini, inla_string_join(secname, "GROUP.FIXED"), 0);
				tmp = iniparser_getdouble(ini, inla_string_join(secname, "GROUP.INITIAL"), 0.0);
				if (!fixed && mb->reuse_mode) {
					tmp = mb->theta_file[mb->theta_counter_file++];
				}
				mb->f_initial[mb->nf] = Realloc(mb->f_initial[mb->nf], mb->f_ntheta[mb->nf] + 1, double);
				SetInitial(mb->f_ntheta[mb->nf], tmp);
				if (mb->f_group_model[mb->nf] == G_AR1 || mb->f_group_model[mb->nf] == G_EXCHANGEABLE) {
					HYPER_INIT(group_rho_intern, tmp);
					if (mb->verbose) {
						printf("\t\tinitialise group_rho_intern[%g]\n", tmp);
						printf("\t\tgroup.fixed=[%1d]\n", fixed);
					}
				} else {
					HYPER_INIT(group_prec_intern, tmp);
					if (mb->verbose) {
						printf("\t\tinitialise group_prec_intern[%g]\n", tmp);
						printf("\t\tgroup.fixed=[%1d]\n", fixed);
					}
				}
				// P(mb->nf);
				// P(mb->f_ntheta[mb->nf]);
				// P(mb->f_fixed[mb->nf][3]);

				mb->f_theta[mb->nf] = Realloc(mb->f_theta[mb->nf], mb->f_ntheta[mb->nf] + 1, double **);
				mb->f_fixed[mb->nf] = Realloc(mb->f_fixed[mb->nf], mb->f_ntheta[mb->nf] + 1, int);
				mb->f_prior[mb->nf] = Realloc(mb->f_prior[mb->nf], mb->f_ntheta[mb->nf] + 1, Prior_tp);

				if (mb->f_group_model[mb->nf] == G_AR1 || mb->f_group_model[mb->nf] == G_EXCHANGEABLE) {
					mb->f_theta[mb->nf][mb->f_ntheta[mb->nf]] = group_rho_intern;
				} else {
					mb->f_theta[mb->nf][mb->f_ntheta[mb->nf]] = group_prec_intern;
				}

				mb->f_fixed[mb->nf][mb->f_ntheta[mb->nf]] = fixed;

				switch (mb->f_group_model[mb->nf]) {
				case G_EXCHANGEABLE:
					inla_read_prior_group(mb, ini, sec, &(mb->f_prior[mb->nf][mb->f_ntheta[mb->nf]]), "GAUSSIAN-group");
					mb->f_ntheta[mb->nf]++;
					break;

				case G_AR1:
					inla_read_prior_group(mb, ini, sec, &(mb->f_prior[mb->nf][mb->f_ntheta[mb->nf]]), "GAUSSIAN-rho");
					mb->f_ntheta[mb->nf]++;
					break;

				case G_RW1:
				case G_RW2:
				case G_BESAG:
					inla_read_prior_group(mb, ini, sec, &(mb->f_prior[mb->nf][mb->f_ntheta[mb->nf]]), "LOGGAMMA");
					mb->f_ntheta[mb->nf]++;
					break;

				default:
					assert(0 == 1);
					abort();
				}

				if (!fixed) {
					/*
					 * add this \theta 
					 */
					mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
					mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
					mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
					mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);

					if (mb->f_group_model[mb->nf] == G_AR1 || mb->f_group_model[mb->nf] == G_EXCHANGEABLE) {
						GMRFLib_sprintf(&msg, "Group rho_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
						mb->theta_tag[mb->ntheta] = msg;
						GMRFLib_sprintf(&msg, "GroupRho for %s", (secname ? secname : mb->f_tag[mb->nf]));
						mb->theta_tag_userscale[mb->ntheta] = msg;
					} else {
						GMRFLib_sprintf(&msg, "Group prec_intern for %s", (secname ? secname : mb->f_tag[mb->nf]));
						mb->theta_tag[mb->ntheta] = msg;
						GMRFLib_sprintf(&msg, "GroupPrec for %s", (secname ? secname : mb->f_tag[mb->nf]));
						mb->theta_tag_userscale[mb->ntheta] = msg;
					}

					GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], mb->f_ntheta[mb->nf] - 1);
					mb->theta_dir[mb->ntheta] = msg;
					if (mb->f_group_model[mb->nf] == G_AR1 || mb->f_group_model[mb->nf] == G_EXCHANGEABLE) {
						mb->theta[mb->ntheta] = group_rho_intern;
					} else {
						mb->theta[mb->ntheta] = group_prec_intern;
					}

					mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
					mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);

					switch (mb->f_group_model[mb->nf]) {
					case G_EXCHANGEABLE:
						mb->theta_map[mb->ntheta] = map_group_rho;
						// need to add a pointer that stays fixed, mb->theta_map_arg[mb->nf] does not!
						int *ngp = NULL;
						ngp = Calloc(1, int);
						*ngp = mb->f_ngroup[mb->nf];
						mb->theta_map_arg[mb->ntheta] = (void *) ngp;
						break;

					case G_AR1:
						mb->theta_map[mb->ntheta] = map_rho;
						mb->theta_map_arg[mb->ntheta] = NULL;
						break;

					case G_RW1:
					case G_RW2:
					case G_BESAG:
						mb->theta_map[mb->ntheta] = map_precision;
						mb->theta_map_arg[mb->ntheta] = NULL;
						break;

					default:
						inla_error_general("this should not happen");
					}

					Prior_tp *pri = &(mb->f_prior[mb->nf][mb->f_ntheta[mb->nf] - 1]);

					mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
					mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
					mb->theta_from[mb->ntheta] = GMRFLib_strdup(pri->from_theta);
					mb->theta_to[mb->ntheta] = GMRFLib_strdup(pri->to_theta);

					mb->ntheta++;
				}
				break;

			case G_AR:
			{
				int ntheta, ntheta_orig;

				ntheta_orig = mb->f_ntheta[mb->nf];
				ntheta = mb->f_group_order[mb->nf] + 1;
				assert(ntheta <= AR_MAXTHETA + 1 && ntheta >= 1);

				mb->f_prior[mb->nf] = Realloc(mb->f_prior[mb->nf], ntheta_orig + 2, Prior_tp);
				inla_read_prior_group0(mb, ini, sec, &(mb->f_prior[mb->nf][ntheta_orig + 0]), "LOGGAMMA");	// log precision
				inla_read_prior_group1(mb, ini, sec, &(mb->f_prior[mb->nf][ntheta_orig + 1]), "MVNORM");	// the pacf

				mb->f_initial[mb->nf] = Realloc(mb->f_initial[mb->nf], ntheta + ntheta_orig, double);
				if (mb->verbose) {
					printf("\t\tgroup.ntheta = [%1d]\n", ntheta);
				}

				if ((int) mb->f_prior[mb->nf][ntheta_orig + 1].parameters[0] != ntheta - 1) {
					GMRFLib_sprintf(&ptmp, "Dimension of the MVNORM prior is not equal to the order of the GROUP-AR-model: %1d != %1d\n",
							(int) mb->f_prior[mb->nf][ntheta_orig + 0].parameters[0], ntheta - 1);
					inla_error_general(ptmp);
					exit(EXIT_FAILURE);
				}

				/*
				 * mark all possible as read 
				 */
				for (i = 0; i < AR_MAXTHETA + 1; i++) {

					GMRFLib_sprintf(&ctmp, "GROUP.FIXED%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

					GMRFLib_sprintf(&ctmp, "GROUP.INITIAL%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

					GMRFLib_sprintf(&ctmp, "GROUP.PRIOR%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

					GMRFLib_sprintf(&ctmp, "GROUP.PARAMETERS%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

					GMRFLib_sprintf(&ctmp, "GROUP.to.theta%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);

					GMRFLib_sprintf(&ctmp, "GROUP.from.theta%1d", i);
					iniparser_getstring(ini, inla_string_join(secname, ctmp), NULL);
				}

				mb->f_fixed[mb->nf] = Realloc(mb->f_fixed[mb->nf], ntheta + ntheta_orig, int);
				mb->f_theta[mb->nf] = Realloc(mb->f_theta[mb->nf], ntheta + ntheta_orig, double **);

				HYPER_NEW(log_prec, 0.0);
				mb->f_theta[mb->nf][ntheta_orig] = log_prec;
				pacf_intern = Calloc(AR_MAXTHETA, double **);
				for (i = 0; i < AR_MAXTHETA; i++) {
					HYPER_NEW(pacf_intern[i], 0.0);
					mb->f_theta[mb->nf][ntheta_orig + i + 1] = pacf_intern[i];
				}

				/*
				 * then read those we need 
				 */
				for (i = 0; i < ntheta; i++) {
					double theta_initial = 0;

					GMRFLib_sprintf(&ctmp, "GROUP.FIXED%1d", i);
					mb->f_fixed[mb->nf][ntheta_orig + i] = iniparser_getboolean(ini, inla_string_join(secname, ctmp), 0);

					GMRFLib_sprintf(&ctmp, "GROUP.INITIAL%1d", i);
					theta_initial = iniparser_getdouble(ini, inla_string_join(secname, ctmp), theta_initial);
					if (!mb->f_fixed[mb->nf][i] && mb->reuse_mode) {
						theta_initial = mb->theta_file[mb->theta_counter_file++];
					}

					if (i == 0) {
						/*
						 * precision 
						 */
						HYPER_INIT(log_prec, theta_initial);
						if (mb->verbose) {
							printf("\t\tinitialise (log_prec) group.theta[%1d]=[%g]\n", i, theta_initial);
							printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][ntheta_orig + i]);
						}

						if (!mb->f_fixed[mb->nf][ntheta_orig + i]) {
							/*
							 * add this \theta 
							 */
							mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
							mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
							mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
							mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
							GMRFLib_sprintf(&msg, "Group Log precision for %s", (secname ? secname : mb->f_tag[mb->nf]));

							mb->theta_tag[mb->ntheta] = msg;
							GMRFLib_sprintf(&msg, "Group Precision for %s", (secname ? secname : mb->f_tag[mb->nf]));
							mb->theta_tag_userscale[mb->ntheta] = msg;
							GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], ntheta_orig + i + 1);
							mb->theta_dir[mb->ntheta] = msg;

							mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
							mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
							mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][ntheta_orig + 0].from_theta);
							mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][ntheta_orig + 0].to_theta);
							mb->theta[mb->ntheta] = log_prec;
							mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
							mb->theta_map[mb->ntheta] = map_precision;
							mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
							mb->theta_map_arg[mb->ntheta] = NULL;
							mb->ntheta++;
						}
					} else {
						/*
						 * PACF 
						 */
						HYPER_INIT(pacf_intern[i - 1], theta_initial);
						if (mb->verbose) {
							printf("\t\tinitialise (PACF) theta[%1d]=[%g]\n", i, theta_initial);
							printf("\t\tfixed[%1d]=[%1d]\n", i, mb->f_fixed[mb->nf][ntheta_orig + i]);
						}
						if (!mb->f_fixed[mb->nf][ntheta_orig + i]) {
							/*
							 * add this \theta 
							 */
							mb->theta = Realloc(mb->theta, mb->ntheta + 1, double **);
							mb->theta_tag = Realloc(mb->theta_tag, mb->ntheta + 1, char *);
							mb->theta_tag_userscale = Realloc(mb->theta_tag_userscale, mb->ntheta + 1, char *);
							mb->theta_dir = Realloc(mb->theta_dir, mb->ntheta + 1, char *);
							GMRFLib_sprintf(&msg, "Group Intern PACF%1d for %s", i, (secname ? secname : mb->f_tag[mb->nf]));

							mb->theta_tag[mb->ntheta] = msg;
							GMRFLib_sprintf(&msg, "Group PACF%1d for %s", i, (secname ? secname : mb->f_tag[mb->nf]));
							mb->theta_tag_userscale[mb->ntheta] = msg;
							GMRFLib_sprintf(&msg, "%s-parameter%1d", mb->f_dir[mb->nf], ntheta_orig + i + 1);
							mb->theta_dir[mb->ntheta] = msg;

							mb->theta_from = Realloc(mb->theta_from, mb->ntheta + 1, char *);
							mb->theta_to = Realloc(mb->theta_to, mb->ntheta + 1, char *);
							mb->theta_from[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][ntheta_orig + 1].from_theta);
							mb->theta_to[mb->ntheta] = GMRFLib_strdup(mb->f_prior[mb->nf][ntheta_orig + 1].to_theta);
							mb->theta[mb->ntheta] = pacf_intern[i - 1];
							mb->theta_map = Realloc(mb->theta_map, mb->ntheta + 1, map_func_tp *);
							mb->theta_map[mb->ntheta] = map_phi;
							mb->theta_map_arg = Realloc(mb->theta_map_arg, mb->ntheta + 1, void *);
							mb->theta_map_arg[mb->ntheta] = NULL;
							mb->ntheta++;
						}
					}
				}
				break;
			}

			default:
				assert(0 == 1);
			}


			/*
			 * make required changes.  oops, the rankdef is for the size-n model, not the size-N one! 
			 */
			int ng = mb->f_ngroup[mb->nf];
			int Norig = mb->f_N[mb->nf];
			GMRFLib_graph_tp *g;

			inla_make_group_graph(&g, mb->f_graph[mb->nf], ng, mb->f_group_model[mb->nf], mb->f_group_cyclic[mb->nf], mb->f_group_order[mb->nf],
					      mb->f_group_graph[mb->nf]);
			GMRFLib_free_graph(mb->f_graph[mb->nf]);
			mb->f_graph[mb->nf] = g;

			/*
			 * make the constraints 
			 */
			GMRFLib_constr_tp *c;
			c = inla_make_constraint2(mb->f_N[mb->nf], mb->f_ngroup[mb->nf], mb->f_sumzero[mb->nf], mb->f_constr[mb->nf]);
			if (c) {
				mb->f_sumzero[mb->nf] = 0;
				Free(mb->f_constr[mb->nf]);
				mb->f_constr[mb->nf] = c;
			}

			/*
			 * redefine the N's, also change the rankdef as its defined for `n'. 
			 */
			mb->f_n[mb->nf] *= ng;
			mb->f_N[mb->nf] *= ng;
			mb->f_rankdef[mb->nf] *= ng;

			int adj = iniparser_getint(ini, inla_string_join(secname, "GROUP.ADJUST.FOR.CON.COMP"), 1);
			int std = iniparser_getint(ini, inla_string_join(secname, "GROUP.SCALE.MODEL"), 0);

			/*
			 * setup the new Qfunc++ 
			 */
			inla_group_def_tp *def = Calloc(1, inla_group_def_tp);

			def->N = Norig;
			def->ngroup = ng;
			def->cyclic = mb->f_group_cyclic[mb->nf];
			def->graph = mb->f_group_graph[mb->nf];
			def->type = mb->f_group_model[mb->nf];
			def->Qfunc = mb->f_Qfunc[mb->nf];
			mb->f_Qfunc[mb->nf] = Qfunc_group;
			def->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
			mb->f_Qfunc_arg[mb->nf] = (void *) def;
			def->group_rho_intern = group_rho_intern;
			def->group_prec_intern = group_prec_intern;
			if (mb->f_group_model[mb->nf] == G_RW1 || mb->f_group_model[mb->nf] == G_RW2) {
				if (def->cyclic) {
					/*
					 * as cyclic is only implemented for GMRFLib_rw()
					 */
					def->rwdef = Calloc(1, GMRFLib_rwdef_tp);
					def->rwdef->n = ng;
					def->rwdef->order = (mb->f_group_model[mb->nf] == G_RW1 ? 1 : 2);
					def->rwdef->cyclic = mb->f_group_cyclic[mb->nf];
					def->rwdef->prec = Calloc(1, double);
					def->rwdef->prec[0] = 1.0;
					def->rwdef->log_prec = NULL;
					def->rwdef->log_prec_omp = NULL;
					if (std) {
						char *err;
						GMRFLib_sprintf(&err, "Group: cannot scale.model with option cylic=TRUE. Contact developers.");
						inla_error_general(err);
						exit(1);
					}
				} else {
					/*
					 * otherwise, we use the general function
					 */
					def->crwdef = Calloc(1, GMRFLib_crwdef_tp);
					def->crwdef->n = ng;
					def->crwdef->order = (mb->f_group_model[mb->nf] == G_RW1 ? 1 : 2);
					def->crwdef->prec = Calloc(1, double);
					def->crwdef->prec[0] = 1.0;
					def->crwdef->log_prec = NULL;
					def->crwdef->log_prec_omp = NULL;
					def->crwdef->layout = GMRFLib_CRW_LAYOUT_SIMPLE;
					def->crwdef->position = Calloc(ng, double);
					int kk;
					for (kk = 0; kk < ng; kk++) {
						def->crwdef->position[kk] = (double) kk;
					}
					if (std) {
						GMRFLib_crw_scale((void *) def->crwdef);
					}
					if (mb->verbose) {
						printf("\t\tgroup.scale.model[%1d]\n", std);
						if (std) {
							printf("\t\tgroup.scale.model: prec_scale[%g]\n", def->crwdef->prec_scale[0]);
						}
					}
				}

			} else if (mb->f_group_model[mb->nf] == G_AR) {
				def->ardef = Calloc(1, ar_def_tp);
				def->ardef->n = mb->f_ngroup[mb->nf];
				def->ardef->p = mb->f_group_order[mb->nf];
				def->ardef->log_prec = log_prec;
				def->ardef->pacf_intern = pacf_intern;
				def->ardef->hold_pacf_intern = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
				def->ardef->hold_Q = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
				def->ardef->hold_Qmarg = Calloc(ISQR(GMRFLib_MAX_THREADS), double *);
				for (i = 0; i < ISQR(GMRFLib_MAX_THREADS); i++) {
					def->ardef->hold_pacf_intern[i] = Calloc(def->ardef->p, double);
					for (j = 0; j < def->ardef->p; j++) {
						def->ardef->hold_pacf_intern[i][j] = GMRFLib_uniform();
					}
				}
			} else if (mb->f_group_model[mb->nf] == G_BESAG) {
				def->besagdef = Calloc(1, inla_besag_Qfunc_arg_tp);
				def->besagdef->graph = mb->f_group_graph[mb->nf];
				if (std) {
					inla_besag_scale((void *) def->besagdef, adj);
				}
				if (mb->verbose) {
					printf("\t\tgroup.scale.model[%1d]\n", std);
					printf("\t\tgroup.adjust.for.con.comp[%1d]\n", std);
					if (std) {
						printf("\t\tgroup.scale.model: prec_scale[%g]\n", def->besagdef->prec_scale[0]);
					}
				}
			} else {
				def->rwdef = NULL;
				def->crwdef = NULL;
				def->ardef = NULL;
				def->besagdef = NULL;
			}

			if (mb->f_bfunc2[mb->nf]) {
				/*
				 * then revise the contents
				 */
				mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
				mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
				mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
				mb->f_bfunc2[mb->nf]->ngroup = ng;
			}
		}

		/*
		 * Do the replicate stuff; this is nice hack! 
		 */
		int rep = mb->f_nrep[mb->nf];
		if (rep > 1) {
			inla_replicate_tp *rep_arg = Calloc(1, inla_replicate_tp);
			rep_arg->Qfunc = mb->f_Qfunc[mb->nf];
			rep_arg->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
			rep_arg->n = mb->f_N[mb->nf];
			inla_replicate_graph(&(mb->f_graph[mb->nf]), rep);	/* this also free the old one */
			mb->f_Qfunc[mb->nf] = Qfunc_replicate;
			mb->f_Qfunc_arg[mb->nf] = (void *) rep_arg;

			GMRFLib_constr_tp *c;
			c = inla_make_constraint2(mb->f_N[mb->nf], mb->f_nrep[mb->nf], mb->f_sumzero[mb->nf], mb->f_constr[mb->nf]);
			if (c) {
				mb->f_sumzero[mb->nf] = 0;
				Free(mb->f_constr[mb->nf]);
				mb->f_constr[mb->nf] = c;
			}
			// GMRFLib_print_constr(stdout, c, mb->f_graph[mb->nf]);

			if (mb->f_bfunc2[mb->nf]) {
				/*
				 * Then revise the contents
				 */
				mb->f_bfunc2[mb->nf]->graph = mb->f_graph[mb->nf];
				mb->f_bfunc2[mb->nf]->Qfunc = mb->f_Qfunc[mb->nf];
				mb->f_bfunc2[mb->nf]->Qfunc_arg = mb->f_Qfunc_arg[mb->nf];
				mb->f_bfunc2[mb->nf]->nreplicate = rep;
			}
		}
		mb->f_Ntotal[mb->nf] = mb->f_N[mb->nf] * rep;
	} else {
		mb->f_Ntotal[mb->nf] = -1;		       /* yes this is set later */
	}

	mb->nf++;
#undef SET
#undef OneOf
#undef OneOf2
#undef OneOf3
#undef SetInitial
	return INLA_OK;
}
double iid_mfunc(int idx, void *arg)
{
	return 1.0 + idx;
}
double Qfunc_copy_part00(int i, int j, void *arg)
{
	inla_copy_arg_tp *a = (inla_copy_arg_tp *) arg;

	if (i == j) {
		double beta = a->map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);
		return a->Qfunc(i, j, a->Qfunc_arg) + a->precision * SQR(beta);
	} else {
		return a->Qfunc(i, j, a->Qfunc_arg);
	}
}
double Qfunc_copy_part01(int i, int j, void *arg)
{
	inla_copy_arg_tp *a = (inla_copy_arg_tp *) arg;
	double beta = a->map_beta(a->beta[GMRFLib_thread_id][0], MAP_FORWARD, a->map_beta_arg);

	return -a->precision * beta;
}
double Qfunc_copy_part11(int i, int j, void *arg)
{
	inla_copy_arg_tp *a = (inla_copy_arg_tp *) arg;

	return a->precision;
}
int inla_add_copyof(inla_tp * mb)
{
	int i, k, kk, kkk, debug = 0, nf = mb->nf;
	char *msg;

	for (k = 0; k < nf; k++) {
		if (mb->f_id[k] == F_COPY) {
			if (debug) {
				printf("ffield %d is F_COPY\n", k);
			}

			kk = find_tag(mb, mb->f_of[k]);
			if (kk < 0 || k == kk) {
				GMRFLib_sprintf(&msg, "ffield %1d is F_COPY and a copy of %s which is not found", k, mb->f_of[k]);
				inla_error_general(msg);
				exit(EXIT_FAILURE);
			}
			if (mb->f_id[kk] == F_COPY && kk > k) {
				GMRFLib_sprintf(&msg, "ffield [%s] is a copy of a (later defined) F_COPY field [%s]; please swap", mb->f_tag[k], mb->f_tag[kk]);
				inla_error_general(msg);
				exit(EXIT_FAILURE);
			}

			if (mb->f_same_as[k]) {
				kkk = find_tag(mb, mb->f_same_as[k]);
				if (kkk < 0) {
					GMRFLib_sprintf(&msg, "ffield %1d is F_COPY but same.as=[%s] is not found", k, mb->f_same_as[k]);
					inla_error_general(msg);
					exit(EXIT_FAILURE);
				}
				if (mb->f_id[kkk] != F_COPY) {
					GMRFLib_sprintf(&msg, "ffield [%s] is a copy of [%s], but same.as=[%s] which is not F_COPY\n", mb->f_tag[k], mb->f_tag[kk],
							mb->f_same_as[k]);
					inla_error_general(msg);
					exit(EXIT_FAILURE);
				}
				if (kkk == k) {
					GMRFLib_sprintf(&msg, "ffield [%s] is a copy of [%s], but same.as=[%s] which is not allowed.\n",
							mb->f_tag[k], mb->f_tag[kk], mb->f_same_as[k]);
					inla_error_general(msg);
					exit(EXIT_FAILURE);
				}
				if (kkk > k) {
					GMRFLib_sprintf(&msg, "ffield [%s] is a copy of [%s], but same.as=[%s] which is after; please swap. %1d > %1d\n",
							mb->f_tag[k], mb->f_tag[kk], mb->f_same_as[k], kkk, k);
					inla_error_general(msg);
					exit(EXIT_FAILURE);
				}
			} else {
				kkk = k;
			}

			if (debug) {
				if (mb->f_same_as[k]) {
					printf("found name %s at ffield %1d [same.as %s = ffield %1d]\n", mb->f_of[k], kk, mb->f_same_as[k], kkk);
				} else {
					printf("found name %s at ffield %1d\n", mb->f_of[k], kk);
				}
			}

			/*
			 * this is required! 
			 */
			if (!mb->ff_Qfunc) {
				mb->ff_Qfunc = Calloc(nf, GMRFLib_Qfunc_tp **);
				mb->ff_Qfunc_arg = Calloc(nf, void **);
				for (i = 0; i < nf; i++) {
					mb->ff_Qfunc[i] = Calloc(nf, GMRFLib_Qfunc_tp *);
					mb->ff_Qfunc_arg[i] = Calloc(nf, void *);
				}
			}

			/*
			 * yes, just use that size 
			 */
			GMRFLib_make_linear_graph(&(mb->f_graph[k]), mb->f_Ntotal[kk], 0, 0);
			GMRFLib_free_constr(mb->f_constr[k]);  /* if its any */
			mb->f_constr[k] = NULL;
			mb->f_sumzero[k] = 0;
			mb->f_rankdef[k] = 0;

			inla_copy_arg_tp *arg = Calloc(1, inla_copy_arg_tp);

			arg->Qfunc = mb->f_Qfunc[kk];
			arg->Qfunc_arg = mb->f_Qfunc_arg[kk];
			arg->precision = mb->f_precision[k];
			arg->beta = mb->f_theta[kkk][0];

			arg->map_beta = mb->f_theta_map[kkk][0];
			arg->map_beta_arg = mb->f_theta_map_arg[kkk][0];

			if (0) {
				if (arg->map_beta_arg) {
					printf("range %g %g\n", ((double *) (arg->map_beta_arg))[0], ((double *) (arg->map_beta_arg))[1]);
				}
			}

			/*
			 * zero this out if its not needed anymore 
			 */
			if (k != kkk) {
				mb->f_theta[k] = NULL;
			}

			mb->f_Qfunc[kk] = Qfunc_copy_part00;
			mb->f_Qfunc_arg[kk] = (void *) arg;

			mb->f_Qfunc[k] = Qfunc_copy_part11;
			mb->f_Qfunc_arg[k] = (void *) arg;

			mb->ff_Qfunc[k][kk] = mb->ff_Qfunc[kk][k] = Qfunc_copy_part01;
			mb->ff_Qfunc_arg[k][kk] = mb->ff_Qfunc_arg[kk][k] = (void *) arg;

			mb->f_n[k] = mb->f_n[kk];
			mb->f_N[k] = mb->f_N[kk];
			mb->f_Ntotal[k] = mb->f_Ntotal[kk];
		}
	}
	return 0;
}
inla_iarray_tp *find_all_f(inla_tp * mb, inla_component_tp id)
{
	inla_iarray_tp *ia = Calloc(1, inla_iarray_tp);

	ia->n = count_f(mb, id);
	if (ia->n) {
		int i, j;

		ia->array = Calloc(ia->n, int);
		for (i = j = 0; i < mb->nf; i++) {
			if (mb->f_id[i] == id) {
				ia->array[j++] = i;
			}
		}
		assert(j == ia->n);
	}

	return ia;
}
int find_f(inla_tp * mb, inla_component_tp id)
{
	int i;
	for (i = 0; i < mb->nf; i++) {
		if (mb->f_id[i] == id) {
			return i;
		}
	}
	return -1;
}
int find_tag(inla_tp * mb, const char *name)
{
	int i;
	for (i = 0; i < mb->nf; i++) {
		if (!strcasecmp((const char *) mb->f_tag[i], name))
			return i;
	}
	return -1;
}
int count_f(inla_tp * mb, inla_component_tp id)
{
	int i, n = 0;
	for (i = 0; i < mb->nf; i++) {
		if (mb->f_id[i] == id) {
			n++;
		}
	}
	return n;
}
int inla_parse_linear(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = LINEAR 
	 */
	int i;
	char *filename = NULL, *secname = NULL, default_tag[100];

	if (mb->verbose) {
		printf("\tinla_parse_linear...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\tsection[%s]\n", secname);
	}
	mb->linear_tag = Realloc(mb->linear_tag, mb->nlinear + 1, char *);
	mb->linear_dir = Realloc(mb->linear_dir, mb->nlinear + 1, char *);
	mb->linear_covariate = Realloc(mb->linear_covariate, mb->nlinear + 1, double *);
	mb->linear_precision = Realloc(mb->linear_precision, mb->nlinear + 1, double);
	mb->linear_mean = Realloc(mb->linear_mean, mb->nlinear + 1, double);
	mb->linear_compute = Realloc(mb->linear_compute, mb->nlinear + 1, int);
	mb->linear_output = Realloc(mb->linear_output, mb->nlinear + 1, Output_tp *);
	sprintf(default_tag, "default tag for linear %d", (int) (10000 * GMRFLib_uniform()));
	mb->linear_tag[mb->nlinear] = GMRFLib_strdup((secname ? secname : default_tag));
	mb->linear_dir[mb->nlinear] =
	    GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "DIR"), inla_fnmfix(GMRFLib_strdup(mb->linear_tag[mb->nlinear]))));
	if (mb->verbose) {
		printf("\t\tdir=[%s]\n", mb->linear_dir[mb->nlinear]);
	}
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "COVARIATES"), NULL));
	if (!filename) {
		if (mb->verbose) {
			printf("\t\tfile for covariates=[(NULL)]: set all covariates to 1\n");
		}
		mb->linear_covariate[mb->nlinear] = Calloc(mb->predictor_n, double);

		for (i = 0; i < mb->predictor_n; i++) {
			mb->linear_covariate[mb->nlinear][i] = 1.0;
		}
	} else {
		if (mb->verbose) {
			printf("\t\tfile for covariates=[%s]\n", filename);
		}
		inla_read_data_general(&(mb->linear_covariate[mb->nlinear]), NULL, NULL, filename, mb->predictor_n, 0, 1, mb->verbose, -1.0);
	}
	mb->linear_mean[mb->nlinear] = iniparser_getdouble(ini, inla_string_join(secname, "MEAN"), 0.0);
	if (mb->verbose) {
		printf("\t\tprior mean=[%g]\n", mb->linear_mean[mb->nlinear]);
	}
	mb->linear_precision[mb->nlinear] = iniparser_getdouble(ini, inla_string_join(secname, "PRECISION"), DEFAULT_NORMAL_PRIOR_PRECISION);
	if (mb->verbose) {
		printf("\t\tprior precision=[%g]\n", mb->linear_precision[mb->nlinear]);
	}
	mb->linear_compute[mb->nlinear] = iniparser_getboolean(ini, inla_string_join(secname, "COMPUTE"), 1);
	if (G.mode == INLA_MODE_HYPER) {
		if (mb->linear_compute[mb->nlinear]) {
			fprintf(stderr, "*** Warning: HYPER_MODE require linear_compute[%1d] = 0\n", mb->nlinear);
		}
		mb->linear_compute[mb->nlinear] = 0;
	}
	if (mb->verbose) {
		printf("\t\tcompute=[%1d]\n", mb->linear_compute[mb->nlinear]);
	}
	inla_parse_output(mb, ini, sec, &(mb->linear_output[mb->nlinear]));
	mb->nlinear++;
	return INLA_OK;
}
int inla_setup_ai_par_default(inla_tp * mb)
{
	/*
	 * change some these values to provide a inla-spesific defaults:
	 * 
	 * - the verbose controls the output - use CCD as default integrator, except for ntheta=1, where the GRID is used. 
	 */
	int i;

	if (!mb->ai_par) {
		GMRFLib_default_ai_param(&(mb->ai_par));

		mb->ai_par->gaussian_data = mb->gaussian_data;
		// P(mb->gaussian_data);

		if (!(G.mode == INLA_MODE_HYPER)) {
			/*
			 * default mode 
			 */

			if (mb->verbose) {
				mb->ai_par->fp_log = stdout;
			} else {
				mb->ai_par->fp_log = NULL;
			}
			if (mb->ntheta == 1) {
				mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_GRID;
			} else {
				mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_CCD;
			}

			for (i = 0; i < mb->nds; i++) {
				if (mb->data_sections[i].data_id == L_T) {
					/*
					 * use special options for the additive student-t 
					 */
					mb->ai_par->strategy = GMRFLib_AI_STRATEGY_FIT_SCGAUSSIAN;
					mb->ai_par->linear_correction = GMRFLib_AI_LINEAR_CORRECTION_FAST;
				}
			}
		} else {
			/*
			 * hyperparameter mode: special options 
			 */

			if (mb->verbose) {
				mb->ai_par->fp_log = stdout;
			} else {
				mb->ai_par->fp_log = NULL;
			}
			for (i = 0; i < mb->nds; i++) {
				if (mb->data_sections[i].data_id == L_T) {
					/*
					 * use special options for the additive student-t 
					 */
					mb->ai_par->strategy = GMRFLib_AI_STRATEGY_FIT_SCGAUSSIAN;
					mb->ai_par->linear_correction = GMRFLib_AI_LINEAR_CORRECTION_FAST;
				}
			}
			mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_GRID;
			// mb->ai_par->skip_configurations = GMRFLib_FALSE;
			mb->ai_par->hessian_force_diagonal = GMRFLib_TRUE;
			switch (mb->ntheta) {
			case 0:
			case 1:
				mb->ai_par->dz = 0.75;
				mb->ai_par->diff_log_dens = 6;
				break;

			default:
				mb->ai_par->dz = 1.0;
				mb->ai_par->diff_log_dens = 5;
			}
			mb->ai_par->compute_nparam_eff = GMRFLib_FALSE;
		}
	}
	if (mb->reuse_mode && !mb->reuse_mode_but_restart) {
		mb->ai_par->mode_known = GMRFLib_TRUE;
	}

	return INLA_OK;
}
int inla_parse_INLA(inla_tp * mb, dictionary * ini, int sec, int make_dir)
{
	/*
	 * parse section = INLA 
	 */
	char *secname = NULL, *opt = NULL, *msg = NULL, *filename = NULL, *default_int_strategy = NULL, *defname = NULL, *r, *ctmp;
	double tmp, tmp_ref;

	if (mb->verbose) {
		printf("\tinla_parse_INLA...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\tsection[%s]\n", secname);
	}

	inla_setup_ai_par_default(mb);			       /* most likely already done, but... */

	mb->lc_derived_only = iniparser_getboolean(ini, inla_string_join(secname, "LINCOMB.DERIVED.ONLY"), 1);
	if (mb->verbose) {
		printf("\t\t\tlincomb.derived.only = [%s]\n", (mb->lc_derived_only ? "Yes" : "No"));
	}
	mb->lc_derived_correlation_matrix = iniparser_getboolean(ini, inla_string_join(secname, "LINCOMB.DERIVED.CORRELATION.MATRIX"), 0);
	if (mb->verbose) {
		printf("\t\t\tlincomb.derived.correlation.matrix = [%s]\n", (mb->lc_derived_correlation_matrix ? "Yes" : "No"));
	}

	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "OPTIMISER"), NULL));
	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "OPTIMIZER"), opt));
	if (!opt) {
		mb->ai_par->optimiser = GMRFLib_AI_OPTIMISER_DEFAULT;
	} else if (!strcasecmp(opt, "DEFAULT")) {
		mb->ai_par->optimiser = GMRFLib_AI_OPTIMISER_DEFAULT;
	} else if (!strcasecmp(opt, "DOMIN")) {
		mb->ai_par->optimiser = GMRFLib_AI_OPTIMISER_DOMIN;
	} else if (!strcasecmp(opt, "GSL")) {
		mb->ai_par->optimiser = GMRFLib_AI_OPTIMISER_GSL;
	} else {
		inla_error_field_is_void(__GMRFLib_FuncName, secname, "optimiser", opt);
	}

	/*
	 * if eps. < 0.0 then factory defaults are used. 
	 */
	mb->ai_par->domin_epsx = iniparser_getdouble(ini, inla_string_join(secname, "DOMIN.EPSX"), mb->ai_par->domin_epsx);
	mb->ai_par->domin_epsx = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.X"), mb->ai_par->domin_epsx);

	mb->ai_par->domin_epsf = iniparser_getdouble(ini, inla_string_join(secname, "DOMIN.EPSF"), mb->ai_par->domin_epsf);
	// as this means the rounding error... in domin()
	// mb->ai_par->domin_epsf = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.F"), mb->ai_par->domin_epsf);

	mb->ai_par->domin_epsg = iniparser_getdouble(ini, inla_string_join(secname, "DOMIN.EPSG"), mb->ai_par->domin_epsg);
	mb->ai_par->domin_epsg = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.G"), mb->ai_par->domin_epsg);

	mb->ai_par->gsl_tol = iniparser_getdouble(ini, inla_string_join(secname, "GSL.TOL"), mb->ai_par->gsl_tol);

	mb->ai_par->gsl_step_size = iniparser_getdouble(ini, inla_string_join(secname, "GSL.STEP.SIZE"), mb->ai_par->gsl_step_size);

	mb->ai_par->gsl_epsg = iniparser_getdouble(ini, inla_string_join(secname, "GSL.EPSG"), mb->ai_par->gsl_epsg);
	mb->ai_par->gsl_epsg = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.G"), mb->ai_par->gsl_epsg);

	mb->ai_par->gsl_epsf = iniparser_getdouble(ini, inla_string_join(secname, "GSL.EPSF"), mb->ai_par->gsl_epsf);
	mb->ai_par->gsl_epsf = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.F"), mb->ai_par->gsl_epsf);

	mb->ai_par->gsl_epsx = iniparser_getdouble(ini, inla_string_join(secname, "GSL.EPSX"), mb->ai_par->gsl_epsx);
	mb->ai_par->gsl_epsx = iniparser_getdouble(ini, inla_string_join(secname, "TOLERANCE.X"), mb->ai_par->gsl_epsx);

	mb->ai_par->optpar_abserr_func = iniparser_getdouble(ini, inla_string_join(secname, "ABSERR.FUNC"), mb->ai_par->optpar_abserr_func);
	mb->ai_par->optpar_abserr_func = iniparser_getdouble(ini, inla_string_join(secname, "OPTPAR.ABSERR.FUNC"), mb->ai_par->optpar_abserr_func);

	mb->ai_par->optpar_abserr_step = iniparser_getdouble(ini, inla_string_join(secname, "ABSERR.STEP"), mb->ai_par->optpar_abserr_step);
	mb->ai_par->optpar_abserr_step = iniparser_getdouble(ini, inla_string_join(secname, "OPTPAR.ABSERR.STEP"), mb->ai_par->optpar_abserr_step);

	mb->ai_par->optpar_nr_step_factor = iniparser_getdouble(ini, inla_string_join(secname, "NR.STEP.FACTOR"), mb->ai_par->optpar_nr_step_factor);

	mb->ai_par->mode_known = iniparser_getboolean(ini, inla_string_join(secname, "MODE.KNOWN"), mb->ai_par->mode_known);
	mb->ai_par->restart = iniparser_getint(ini, inla_string_join(secname, "RESTART"), 0);

	if (mb->verbose > 2) {
		ctmp = GMRFLib_strdup("STDOUT");
	} else {
		ctmp = NULL;
	}
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "OPTPAR.FP"), ctmp));

	if (filename) {
		if (!strcasecmp(filename, "STDOUT")) {
			mb->ai_par->optpar_fp = stdout;
		} else if (!strcasecmp(filename, "STDERR")) {
			mb->ai_par->optpar_fp = stderr;
		} else if (!strcasecmp(filename, "NULL")) {
			mb->ai_par->optpar_fp = NULL;
		} else if (!strcasecmp(filename, "/dev/null")) {
			mb->ai_par->optpar_fp = NULL;
		} else {
			static FILE *fp = NULL;

			inla_fnmfix(filename);
			fp = fopen(filename, "w");
			if (!fp) {
				GMRFLib_sprintf(&msg, "%s: fail to open file[%s]", __GMRFLib_FuncName, filename);
			}
			mb->ai_par->optpar_fp = fp;
		}
	}

	switch (mb->ai_par->int_strategy) {
	case GMRFLib_AI_INT_STRATEGY_GRID:
		default_int_strategy = GMRFLib_strdup("GMRFLib_AI_INT_STRATEGY_GRID");
		break;

	case GMRFLib_AI_INT_STRATEGY_CCD:
		default_int_strategy = GMRFLib_strdup("GMRFLib_AI_INT_STRATEGY_CCD");
		break;

	default:
		GMRFLib_ASSERT(0 == 1, GMRFLib_ESNH);
	}

	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "STRATEGY"), NULL));
	if (opt) {
		if (!strcasecmp(opt, "GMRFLib_AI_STRATEGY_GAUSSIAN") || !strcasecmp(opt, "GAUSSIAN")) {
			mb->ai_par->strategy = GMRFLib_AI_STRATEGY_GAUSSIAN;
		} else if (!strcasecmp(opt, "GMRFLib_AI_STRATEGY_MEANSKEWCORRECTED_GAUSSIAN") ||
			   !strcasecmp(opt, "MEANSKEWCORRECTED_GAUSSIAN") ||
			   !strcasecmp(opt, "SLA") || !strcasecmp(opt, "SIMPLIFIED_LAPLACE") || !strcasecmp(opt, "SIMPLIFIED.LAPLACE")) {
			mb->ai_par->strategy = GMRFLib_AI_STRATEGY_MEANSKEWCORRECTED_GAUSSIAN;
		} else if (!strcasecmp(opt, "GMRFLib_AI_STRATEGY_FIT_SCGAUSSIAN") ||
			   !strcasecmp(opt, "FIT_SCGAUSSIAN") ||
			   !strcasecmp(opt, "FIT.SCGAUSSIAN") || !strcasecmp(opt, "SCGAUSSIAN") || !strcasecmp(opt, "LAPLACE") || !strcasecmp(opt, "LA")) {
			mb->ai_par->strategy = GMRFLib_AI_STRATEGY_FIT_SCGAUSSIAN;
		} else if (!strcasecmp(opt, "GMRFLib_AI_STRATEGY_MEANCORRECTED_GAUSSIAN") || !strcasecmp(opt, "MEANCORRECTED_GAUSSIAN")) {
			mb->ai_par->strategy = GMRFLib_AI_STRATEGY_MEANCORRECTED_GAUSSIAN;
		} else if (!strcasecmp(opt, "GMRFLib_AI_STRATEGY_MEANSKEWCORRECTED_GAUSSIAN") ||
			   !strcasecmp(opt, "MEANSKEWCORRECTED_GAUSSIAN") ||
			   !strcasecmp(opt, "SLA") || !strcasecmp(opt, "SIMPLIFIED_LAPLACE") || !strcasecmp(opt, "SIMPLIFIED.LAPLACE")) {
			mb->ai_par->strategy = GMRFLib_AI_STRATEGY_MEANSKEWCORRECTED_GAUSSIAN;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "strategy", opt);
		}
	}

	mb->ai_par->fast = iniparser_getboolean(ini, inla_string_join(secname, "FAST"), mb->ai_par->fast);
	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "LINEAR.CORRECTION"), NULL));
	if (opt) {
		if (!strcasecmp(opt, "GMRFLib_AI_LINEAR_CORRECTION_CENTRAL_DIFFERENCE") || !strcasecmp(opt, "CENTRAL_DIFFERENCE")) {
			mb->ai_par->linear_correction = GMRFLib_AI_LINEAR_CORRECTION_CENTRAL_DIFFERENCE;
		} else if (!strcasecmp(opt, "GMRFLib_AI_LINEAR_CORRECTION_FAST") || !strcasecmp(opt, "FAST")
			   || !strcasecmp(opt, "1") || !strcasecmp(opt, "ON") || !strcasecmp(opt, "YES") || !strcasecmp(opt, "TRUE")) {
			mb->ai_par->linear_correction = GMRFLib_AI_LINEAR_CORRECTION_FAST;
		} else if (!strcasecmp(opt, "GMRFLib_AI_LINEAR_CORRECTION_OFF") || !strcasecmp(opt, "OFF") || !strcasecmp(opt, "NO") ||
			   !strcasecmp(opt, "0") || !strcasecmp(opt, "FALSE")) {
			mb->ai_par->linear_correction = GMRFLib_AI_LINEAR_CORRECTION_OFF;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "linear_correction", opt);
		}
	}
	mb->ai_par->n_points = iniparser_getint(ini, inla_string_join(secname, "N.POINTS"), mb->ai_par->n_points);
	mb->ai_par->n_points = iniparser_getint(ini, inla_string_join(secname, "NPOINTS"), mb->ai_par->n_points);
	mb->ai_par->step_len = iniparser_getdouble(ini, inla_string_join(secname, "STEP.LEN"), mb->ai_par->step_len);
	mb->ai_par->stencil = iniparser_getint(ini, inla_string_join(secname, "STENCIL"), mb->ai_par->stencil);
	mb->ai_par->cutoff = iniparser_getdouble(ini, inla_string_join(secname, "CUTOFF"), mb->ai_par->cutoff);
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FP.LOG"), NULL));
	if (filename) {
		if (!strcasecmp(filename, "STDOUT")) {
			mb->ai_par->fp_log = stdout;
		} else if (!strcasecmp(filename, "STDERR")) {
			mb->ai_par->fp_log = stderr;
		} else if (!strcasecmp(filename, "NULL")) {
			mb->ai_par->fp_log = NULL;
		} else if (!strcasecmp(filename, "/dev/null")) {
			mb->ai_par->fp_log = NULL;
		} else {
			static FILE *fp = NULL;

			inla_fnmfix(filename);
			fp = fopen(filename, "w");
			if (!fp) {
				GMRFLib_sprintf(&msg, "%s: fail to open file[%s]", __GMRFLib_FuncName, filename);
			}
			mb->ai_par->fp_log = fp;
		}
	}
	GMRFLib_sprintf(&defname, ".inla_hyper");
	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FP.HYPERPARAM"), defname));
	Free(defname);
	if (!filename) {
		filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FP.HYPERPARAM"), NULL));
	}
	if (filename) {
		if (!strcasecmp(filename, "STDOUT")) {
			mb->ai_par->fp_hyperparam = stdout;
		} else if (!strcasecmp(filename, "STDERR")) {
			mb->ai_par->fp_hyperparam = stderr;
		} else if (!strcasecmp(filename, "NULL")) {
			mb->ai_par->fp_hyperparam = NULL;
		} else if (!strcasecmp(filename, "/dev/null")) {
			mb->ai_par->fp_hyperparam = NULL;
		} else {
			static FILE *fp = NULL;

			inla_fnmfix(filename);
			fp = fopen(filename, "w");
			if (!fp) {
				GMRFLib_sprintf(&msg, "%s: fail to open file[%s]", __GMRFLib_FuncName, filename);
			}
			mb->ai_par->fp_hyperparam = fp;
		}
	}
	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "INT.STRATEGY"), default_int_strategy));
	if (opt) {
		if (!strcasecmp(opt, "GMRFLib_AI_INT_STRATEGY_GRID") || !strcasecmp(opt, "GRID")) {
			mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_GRID;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INT_STRATEGY_CCD") || !strcasecmp(opt, "CCD")) {
			mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_CCD;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INT_STRATEGY_EMPIRICAL_BAYES")
			   || !strcasecmp(opt, "EMPIRICAL_BAYES") || !strcasecmp(opt, "EB")) {
			mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_EMPIRICAL_BAYES;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "int_strategy", opt);
		}
	}
	if (G.mode == INLA_MODE_HYPER) {
		if (mb->ai_par->int_strategy != GMRFLib_AI_INT_STRATEGY_GRID) {
			fprintf(stderr, "*** Warning: HYPER_MODE require int_strategy = GMRFLib_AI_INT_STRATEGY_GRID\n");
		}
		mb->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_GRID;
	}

	mb->ai_par->f0 = iniparser_getdouble(ini, inla_string_join(secname, "F0"), mb->ai_par->f0);
	tmp = iniparser_getdouble(ini, inla_string_join(secname, "DZ"), mb->ai_par->dz);
	if (G.mode == INLA_MODE_HYPER && tmp > mb->ai_par->dz) {
		/*
		 * cannot set it to a larger value 
		 */
		fprintf(stderr, "*** Warning: HYPER_MODE require dz <= %f\n", mb->ai_par->dz);
	} else {
		mb->ai_par->dz = tmp;
	}
	mb->ai_par->adjust_weights = iniparser_getboolean(ini, inla_string_join(secname, "ADJUST.WEIGHTS"), mb->ai_par->adjust_weights);

	tmp_ref = mb->ai_par->diff_log_dens;
	mb->ai_par->diff_log_dens = iniparser_getdouble(ini, inla_string_join(secname, "DIFF.LOG.DENS"), mb->ai_par->diff_log_dens);
	mb->ai_par->diff_log_dens = iniparser_getdouble(ini, inla_string_join(secname, "DIFF.LOGDENS"), mb->ai_par->diff_log_dens);
	if (G.mode == INLA_MODE_HYPER && mb->ai_par->diff_log_dens < tmp_ref) {
		fprintf(stderr, "*** Warning: HYPER_MODE require diff_log_dens >= %f\n", tmp_ref);
		mb->ai_par->diff_log_dens = tmp_ref;
	}
	mb->ai_par->skip_configurations = iniparser_getboolean(ini, inla_string_join(secname, "SKIP.CONFIGURATIONS"), mb->ai_par->skip_configurations);

	if (G.mode == INLA_MODE_HYPER && mb->ai_par->skip_configurations) {
		fprintf(stderr, "*** Warning: HYPER_MODE require skip_configurations = 0\n");
		mb->ai_par->skip_configurations = 0;
	}

	/*
	 * this is a short version for setting both: grad=H hess=sqrt(H)
	 */
	mb->ai_par->gradient_finite_difference_step_len = iniparser_getdouble(ini, inla_string_join(secname, "H"), mb->ai_par->gradient_finite_difference_step_len);

	/*
	 * if H < 0, use central difference.  FIXME LATER!!! 
	 */
	if (mb->ai_par->gradient_finite_difference_step_len < 0.0) {
		mb->ai_par->gradient_finite_difference_step_len = ABS(mb->ai_par->gradient_finite_difference_step_len);
		mb->ai_par->gradient_forward_finite_difference = GMRFLib_FALSE;
	}

	mb->ai_par->hessian_finite_difference_step_len =
	    sqrt(ABS(iniparser_getdouble(ini, inla_string_join(secname, "H"), SQR(mb->ai_par->hessian_finite_difference_step_len))));

	/*
	 * ...which is overrided by the original names 
	 */
	mb->ai_par->gradient_forward_finite_difference =
	    iniparser_getboolean(ini, inla_string_join(secname, "GRADIENT.FORWARD.FINITE.DIFFERENCE"), mb->ai_par->gradient_forward_finite_difference);
	mb->ai_par->gradient_finite_difference_step_len =
	    iniparser_getdouble(ini, inla_string_join(secname, "GRADIENT.FINITE.DIFFERENCE.STEP.LEN"), mb->ai_par->gradient_finite_difference_step_len);
	mb->ai_par->hessian_forward_finite_difference =
	    iniparser_getboolean(ini, inla_string_join(secname, "HESSIAN.FORWARD.FINITE.DIFFERENCE"), mb->ai_par->hessian_forward_finite_difference);
	mb->ai_par->hessian_finite_difference_step_len =
	    iniparser_getdouble(ini, inla_string_join(secname, "HESSIAN.FINITE.DIFFERENCE.STEP.LEN"), mb->ai_par->hessian_finite_difference_step_len);
	mb->ai_par->hessian_force_diagonal = iniparser_getboolean(ini, inla_string_join(secname, "HESSIAN.FORCE.DIAGONAL"), mb->ai_par->hessian_force_diagonal);

	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "INTERPOLATOR"), NULL));
	if (opt) {
		if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_WEIGHTED_DISTANCE") || !strcasecmp(opt, "WEIGHTED_DISTANCE") || !strcasecmp(opt, "WEIGHTED.DISTANCE")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_WEIGHTED_DISTANCE;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_NEAREST") || !strcasecmp(opt, "NEAREST")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_NEAREST;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_LINEAR") || !strcasecmp(opt, "LINEAR")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_LINEAR;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_QUADRATIC") || !strcasecmp(opt, "QUADRATIC")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_QUADRATIC;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_CCD") || !strcasecmp(opt, "CCD")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_CCD;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_CCD_INTEGRATE") || !strcasecmp(opt, "CCDINTEGRATE")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_CCD_INTEGRATE;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_GRIDSUM") || !strcasecmp(opt, "GRIDSUM")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_GRIDSUM;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_AUTO") || !strcasecmp(opt, "AUTO")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_AUTO;
		} else if (!strcasecmp(opt, "GMRFLib_AI_INTERPOLATOR_GAUSSIAN") || !strcasecmp(opt, "GAUSSIAN")) {
			mb->ai_par->interpolator = GMRFLib_AI_INTERPOLATOR_GAUSSIAN;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "interpolator", opt);
		}
	}
	if (mb->ai_par->interpolator == GMRFLib_AI_INTERPOLATOR_GRIDSUM) {
		if (!mb->ai_par->hessian_force_diagonal) {
			GMRFLib_sprintf(&msg, "interpolator=GRIDSUM require hessian_force_diagonal=1 (and skip_configurations=0, recommended)");
			inla_error_general(msg);
		}
	}
	mb->ai_par->do_MC_error_check = iniparser_getboolean(ini, inla_string_join(secname, "DO.MC.ERROR.CHECK"), mb->ai_par->do_MC_error_check);
	mb->ai_par->compute_nparam_eff = iniparser_getboolean(ini, inla_string_join(secname, "COMPUTE.NPARAM.EFF"), mb->ai_par->compute_nparam_eff);

	if (G.mode == INLA_MODE_HYPER) {
		if (mb->ai_par->compute_nparam_eff) {
			fprintf(stderr, "*** Warning: HYPER_MODE require compute_nparam_eff = GMRFLib_FALSE\n");
		}
		mb->ai_par->compute_nparam_eff = GMRFLib_FALSE;
	}

	tmp = iniparser_getboolean(ini, inla_string_join(secname, "HUGE"), -1);
	if (tmp != -1) {
		fprintf(stderr, "\n\n*** Warning *** option control.inla(huge=TRUE) is disabled and obsolete.\n");
		fprintf(stderr, "*** Warning *** use control.compute = list(strategy = \"SMALL|MEDIUM|LARGE|HUGE|DEFAULT\") instead.\n\n");
	}

	GMRFLib_global_node.factor = iniparser_getdouble(ini, inla_string_join(secname, "GLOBAL.NODE.FACTOR"), GMRFLib_global_node.factor);
	assert(GMRFLib_global_node.factor >= 0.0);
	if (mb->verbose) {
		printf("\t\tglobal_node.factor = %.3f\n", GMRFLib_global_node.factor);
	}

	GMRFLib_global_node.degree = iniparser_getdouble(ini, inla_string_join(secname, "GLOBAL.NODE.DEGREE"), GMRFLib_global_node.degree);
	assert(GMRFLib_global_node.degree >= 0);
	if (mb->verbose) {
		printf("\t\tglobal_node.degree = %.1d\n", GMRFLib_global_node.degree);
	}

	memcpy((void *) &(mb->gn), (void *) &GMRFLib_global_node, sizeof(GMRFLib_global_node_tp));

	r = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "REORDERING"), NULL));
	if (mb->verbose) {
		printf("\t\treordering = %s\n", (r ? r : "(default)"));
	}

	if (r) {
		int err;

		/*
		 * both these fail if the reordering is void 
		 */
		err = inla_sread_ints(&G.reorder, 1, r);
		if (err) {
			G.reorder = GMRFLib_reorder_id((const char *) r);
		}
		GMRFLib_reorder = G.reorder;		       /* yes! */
	}

	mb->ai_par->cpo_req_diff_logdens = iniparser_getdouble(ini, inla_string_join(secname, "CPO.REQ.DIFF.LOGDENS"), mb->ai_par->cpo_req_diff_logdens);
	mb->ai_par->cpo_req_diff_logdens = iniparser_getdouble(ini, inla_string_join(secname, "CPO.DIFF"), mb->ai_par->cpo_req_diff_logdens);
	mb->ai_par->cpo_req_diff_logdens = DMAX(0.0, mb->ai_par->cpo_req_diff_logdens);

	mb->ai_par->stupid_search_mode = iniparser_getboolean(ini, inla_string_join(secname, "STUPID.SEARCH"), mb->ai_par->stupid_search_mode);
	mb->ai_par->stupid_search_max_iter = iniparser_getint(ini, inla_string_join(secname, "STUPID.SEARCH.MAX.ITER"), mb->ai_par->stupid_search_max_iter);
	mb->ai_par->stupid_search_factor = iniparser_getdouble(ini, inla_string_join(secname, "STUPID.SEARCH.FACTOR"), mb->ai_par->stupid_search_factor);

	mb->expert_diagonal_emergencey = 0.0;
	mb->expert_diagonal_emergencey = iniparser_getdouble(ini, inla_string_join(secname, "DIAGONAL"), mb->expert_diagonal_emergencey);
	mb->expert_diagonal_emergencey = DMAX(0.0, mb->expert_diagonal_emergencey);
	if (mb->expert_diagonal_emergencey && mb->verbose) {
		printf("\tdiagonal (expert emergency) = %g\n", mb->expert_diagonal_emergencey);
	}

	mb->ai_par->numint_max_fn_eval = iniparser_getint(ini, inla_string_join(secname, "NUMINT.MAXFEVAL"), mb->ai_par->numint_max_fn_eval);
	mb->ai_par->numint_rel_err = iniparser_getdouble(ini, inla_string_join(secname, "NUMINT.RELERR"), mb->ai_par->numint_rel_err);
	mb->ai_par->numint_abs_err = iniparser_getdouble(ini, inla_string_join(secname, "NUMINT.ABSERR"), mb->ai_par->numint_abs_err);

	mb->ai_par->cmin = iniparser_getdouble(ini, inla_string_join(secname, "CMIN"), mb->ai_par->cmin);

	int corr = iniparser_getboolean(ini, inla_string_join(secname, "CORRECT"), 0);
	mb->ai_par->correct = (corr ? Calloc(1, char) : NULL);
	mb->ai_par->correct_verbose = iniparser_getboolean(ini, inla_string_join(secname, "CORRECT.VERBOSE"), mb->ai_par->correct_verbose);
	mb->ai_par->correct_factor = iniparser_getdouble(ini, inla_string_join(secname, "CORRECT.FACTOR"), mb->ai_par->correct_factor);
	opt = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CORRECT.STRATEGY"), NULL));
	if (opt) {
		if (!strcasecmp(opt, "SIMPLIFIED.LAPLACE")) {
			mb->ai_par->correct_strategy = GMRFLib_AI_STRATEGY_MEANCORRECTED_GAUSSIAN;
		} else if (!strcasecmp(opt, "LAPLACE")) {
			mb->ai_par->correct_strategy = GMRFLib_AI_STRATEGY_FIT_SCGAUSSIAN;
		} else {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "correct.strategy", opt);
		}
	}

	if (mb->verbose) {
		GMRFLib_print_ai_param(stdout, mb->ai_par);
	}

	return INLA_OK;
}
int inla_parse_update(inla_tp * mb, dictionary * ini, int sec, int make_dir)
{
	/*
	 * parse section = UPDATE
	 */
	char *secname = NULL, *filename = NULL;
	GMRFLib_matrix_tp *M = NULL;

	if (mb->verbose) {
		printf("\tinla_parse_update...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\tsection[%s]\n", secname);
	}

	filename = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "FILENAME"), NULL));
	if (mb->verbose) {
		printf("\t\tfilename[%s]\n", filename);
	}

	if (filename) {
		M = GMRFLib_read_fmesher_file(filename, (long int) 0, -1);
		mb->update = Calloc(1, inla_update_tp);

		int i = 0, j = 0, nt, k, kk;

		mb->update->ntheta = nt = (int) GMRFLib_matrix_get(i++, j, M);
		if (mb->verbose) {
			printf("\t\tntheta = %1d\n", nt);
		}

		mb->update->theta_mode = Calloc(nt, double);
		for (k = 0; k < nt; k++) {
			mb->update->theta_mode[k] = GMRFLib_matrix_get(i++, j, M);
			if (mb->verbose) {
				printf("\t\ttheta.mode[%1d] = %.10g\n", k, mb->update->theta_mode[k]);
			}
		}
		mb->update->stdev_corr_pos = Calloc(nt, double);
		for (k = 0; k < nt; k++) {
			mb->update->stdev_corr_pos[k] = GMRFLib_matrix_get(i++, j, M);
			if (mb->verbose) {
				printf("\t\tstdev.corr.pos[%1d] = %.10g\n", k, mb->update->stdev_corr_pos[k]);
			}
		}
		mb->update->stdev_corr_neg = Calloc(nt, double);
		for (k = 0; k < nt; k++) {
			mb->update->stdev_corr_neg[k] = GMRFLib_matrix_get(i++, j, M);
			if (mb->verbose) {
				printf("\t\tstdev.corr.neg[%1d] = %.10g\n", k, mb->update->stdev_corr_neg[k]);
			}
		}
		mb->update->sqrt_eigen_values = gsl_vector_calloc((size_t) nt);
		for (k = 0; k < nt; k++) {
			gsl_vector_set(mb->update->sqrt_eigen_values, k, GMRFLib_matrix_get(i++, j, M));
			if (mb->verbose) {
				printf("\t\tsqrt.eigen.values[%1d] = %.10g\n", k, gsl_vector_get(mb->update->sqrt_eigen_values, k));
			}
		}
		mb->update->eigen_vectors = gsl_matrix_calloc((size_t) nt, (size_t) nt);
		for (kk = 0; kk < nt; kk++) {
			for (k = 0; k < nt; k++) {
				/*
				 * column based storage...
				 */
				gsl_matrix_set(mb->update->eigen_vectors, k, kk, GMRFLib_matrix_get(i++, j, M));
				if (mb->verbose) {
					printf("\t\teigenvectors[%1d,%1d] = %.10g\n", k, kk, gsl_matrix_get(mb->update->eigen_vectors, k, kk));
				}
			}
		}
		assert(1 == M->ncol);
		assert(i == M->nrow);
		GMRFLib_matrix_free(M);
	}

	return INLA_OK;
}
int inla_parse_expert(inla_tp * mb, dictionary * ini, int sec)
{
	/*
	 * parse section = expert
	 */
	char *secname = NULL;

	if (mb->verbose) {
		printf("\tinla_parse_expert...\n");
	}
	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (mb->verbose) {
		printf("\t\tsection[%s]\n", secname);
	}

	/*
	 * do error-checking later on 
	 */
	mb->expert_cpo_manual = iniparser_getint(ini, inla_string_join(secname, "CPO.MANUAL"), 0);

	char *str = NULL;
	str = iniparser_getstring(ini, inla_string_join(secname, "CPO.IDX"), str);

	int n = 0;
	int *idx = NULL;
	inla_sread_ints_q(&idx, &n, (const char *) str);
	Free(str);

	mb->expert_n_cpo_idx = n;
	mb->expert_cpo_idx = idx;

	if (mb->verbose) {
		int i;

		printf("\t\t\tcpo.manual=[%1d]\n", mb->expert_cpo_manual);
		for (i = 0; i < mb->expert_n_cpo_idx; i++) {
			printf("\t\t\tcpo.idx=[%1d]\n", mb->expert_cpo_idx[i]);
		}
	}

	/*
	 * joint prior?
	 */
	char *R_HOME = NULL, *Rfile = NULL, *func = NULL;

	R_HOME = iniparser_getstring(ini, inla_string_join(secname, "JP.R_HOME"), R_HOME);
	Rfile = iniparser_getstring(ini, inla_string_join(secname, "JP.RFILE"), Rfile);
	func = iniparser_getstring(ini, inla_string_join(secname, "JP.FUNC"), func);
	if (mb->verbose) {
		printf("\t\t\tjp.R_HOME=[%s]\n", R_HOME);
		printf("\t\t\tjp.Rfile=[%s]\n", Rfile);
		printf("\t\t\tjp.func=[%s]\n", func);
	}
	if (func) {
		GMRFLib_ASSERT(Rfile && R_HOME, GMRFLib_EPARAMETER);
		mb->jp = Calloc(1, inla_jp_tp);
		mb->jp->R_HOME = GMRFLib_strdup(R_HOME);
		mb->jp->Rfile = GMRFLib_strdup(Rfile);
		mb->jp->func = GMRFLib_strdup(func);

	} else {
		mb->jp = NULL;
	}
	Free(R_HOME);
	Free(Rfile);
	Free(func);

	return INLA_OK;
}
double inla_ar1_cyclic_logdet(int N_orig, double phi)
{
	double logdet = 0.0;
	double tpon = 2.0 * M_PI / N_orig;
	int jj;

	for (jj = 0; jj < N_orig; jj++) {
		logdet += log(1.0 + SQR(phi) - phi * (cos(tpon * jj) + cos(tpon * (N_orig - 1.0) * jj)));
	}
	return (logdet);
}

double extra(double *theta, int ntheta, void *argument)
{
	int i, j, count = 0, nfixed = 0, fail, fixed0, fixed1, fixed2, fixed3, debug = 0, evaluate_hyper_prior = 1;

	double val = 0.0, log_precision, log_precision0, log_precision1, rho, rho_intern, beta, beta_intern, logit_rho,
	    group_rho = NAN, group_rho_intern = NAN, ngroup = NAN, normc_g = 0.0, n_orig = NAN, N_orig = NAN, rankdef_orig = NAN,
	    h2_intern, phi, phi_intern, a_intern, dof_intern, logdet, group_prec = NAN, group_prec_intern = NAN, grankdef = 0.0, gcorr = 1.0,
	    log_halflife, log_shape;


	inla_tp *mb = NULL;
	gsl_matrix *Q = NULL;

#define NOT_FIXED(_fx) (!mb->fixed_mode && !mb->_fx)

#define SET_GROUP_RHO(_nt_)						\
	if (mb->f_group_model[i] != G_AR) {				\
		if (mb->f_ngroup[i] == 1) {				\
			assert(mb->f_ntheta[i] == (_nt_));		\
		} else {						\
			assert(mb->f_ntheta[i] == (_nt_)+1);		\
		}							\
	}								\
	ngroup = mb->f_ngroup[i];					\
	n_orig = mb->f_n[i]/ngroup;					\
	N_orig = mb->f_N[i]/ngroup;					\
	rankdef_orig = mb->f_rankdef[i]/ngroup;				\
	grankdef = 0.0;							\
	if (mb->f_ngroup[i] > 1) {					\
		if (mb->f_group_model[i] == G_AR) {			\
			double _log_precision, *_pacf, *_pacf_intern;	\
			int _p;						\
									\
			_p = mb->f_group_order[i];			\
			if (NOT_FIXED(f_fixed[i][(_nt_) + 0])) {	\
				_log_precision = theta[count];		\
				count++;				\
			} else {					\
				_log_precision = mb->f_theta[i][(_nt_) + 0][GMRFLib_thread_id][0]; \
			}						\
			_pacf = Calloc(_p, double);			\
			_pacf_intern = Calloc(_p, double);		\
			for (j = 0; j < _p; j++) {			\
				if (NOT_FIXED(f_fixed[i][(_nt_) + j + 1])) { \
					_pacf_intern[j] = theta[count];	\
					count++;			\
				} else {				\
					_pacf_intern[j] = mb->f_theta[i][(_nt_) + j + 1][GMRFLib_thread_id][0]; \
				}					\
				_pacf[j] = ar_map_pacf(_pacf_intern[j], MAP_FORWARD, NULL); \
			}						\
			double _marginal_prec, _conditional_prec, *_marginal_Q, *_param, *_zero; \
			_marginal_Q = Calloc(ISQR(_p), double);		\
			ar_marginal_distribution(_p, _pacf, &_marginal_prec, _marginal_Q); \
			_conditional_prec = exp(_log_precision) / _marginal_prec; \
			_param = Calloc(1 + _p + ISQR(_p), double);	\
			_zero = Calloc(_p, double);			\
			_param[0] = _p;					\
			for (j = 0; j < ISQR(_p); j++) {		\
				_param[1 + _p + j] = _marginal_Q[j] * exp(_log_precision); \
			}						\
			normc_g = priorfunc_mvnorm(_zero, _param) + (ngroup - _p) * (-0.5 * log(2 * M_PI) + 0.5 * log(_conditional_prec)); \
			normc_g -= LOG_NORMC_GAUSSIAN * ngroup; /* This term goes into the main code therefore its removed here	*/ \
			normc_g *= (N_orig - rankdef_orig);		\
			if (NOT_FIXED(f_fixed[i][(_nt_) + 0])) {	\
				val += PRIOR_EVAL(mb->f_prior[i][(_nt_) + 0], &_log_precision); \
			}						\
			val += PRIOR_EVAL(mb->f_prior[i][(_nt_) + 1], _pacf_intern); \
			Free(_param);					\
			Free(_zero);					\
			Free(_pacf);					\
			Free(_pacf_intern);				\
			Free(_marginal_Q);				\
		} else {						\
			if (NOT_FIXED(f_fixed[i][(_nt_)])){		\
				group_rho_intern = group_prec_intern = theta[count]; \
				count++;				\
			} else {					\
				group_rho_intern = group_prec_intern= mb->f_theta[i][(_nt_)][GMRFLib_thread_id][0]; \
			}						\
			if (mb->f_group_model[i] == G_EXCHANGEABLE) {	\
				int ingroup = (int) ngroup;		\
				group_rho = map_group_rho(group_rho_intern, MAP_FORWARD, (void *) &ingroup); \
				normc_g = - 0.5 * (log(1.0+(ngroup - 1.0) * group_rho) + (ngroup-1)*log(1.0-group_rho)); \
				if (NOT_FIXED(f_fixed[i][(_nt_)])) {	\
					val += PRIOR_EVAL(mb->f_prior[i][(_nt_)], &group_rho_intern); \
				}					\
			} else if (mb->f_group_model[i] == G_AR1) {	\
				group_rho = map_rho(group_rho_intern, MAP_FORWARD, NULL); \
				if (mb->f_group_cyclic[i]) {		\
					normc_g = - (ngroup - 0.0) * 0.5 * log(1.0 - SQR(group_rho)) + 0.5 * inla_ar1_cyclic_logdet(ngroup, group_rho); \
				} else {				\
					normc_g = - (ngroup - 1.0) * 0.5 * log(1.0 - SQR(group_rho)); \
				}					\
				if (NOT_FIXED(f_fixed[i][(_nt_)])) {	\
					val += PRIOR_EVAL(mb->f_prior[i][(_nt_)], &group_rho_intern); \
				}					\
			} else if (mb->f_group_model[i] == G_RW1 || mb->f_group_model[i] == G_RW2 || mb->f_group_model[i] == G_BESAG) { \
				grankdef = (mb->f_group_model[i] == G_RW1 || mb->f_group_model[i] == G_BESAG ? 1.0 : 2.0); \
				group_prec = map_precision(group_prec_intern, MAP_FORWARD, NULL); \
				normc_g = 0.5 * (ngroup - grankdef) * log(group_prec); \
				if (NOT_FIXED(f_fixed[i][(_nt_)])) {	\
					val += PRIOR_EVAL(mb->f_prior[i][(_nt_)], &group_prec_intern); \
				}					\
			} else {					\
				abort();				\
			}						\
			normc_g *= (N_orig - rankdef_orig);		\
		}							\
		gcorr = 1.0 - grankdef / mb->f_ngroup[i];		\
	} else {							\
		group_rho = group_rho_intern = 0.0;			\
		normc_g = 0.0;						\
		ngroup = 1.0;						\
		n_orig = mb->f_n[i];					\
		N_orig = mb->f_N[i];					\
		rankdef_orig = mb->f_rankdef[i];			\
		grankdef = 0.0;						\
		gcorr = 1.0;						\
	}


	mb = (inla_tp *) argument;

	/*
	 * this will evaluate all the hyperparameters and disable EVAL_PRIOR...
	 */
	if (mb->update) {
		val += inla_update_density(theta, mb->update);
		evaluate_hyper_prior = 0;
	}
	// joint prior evaluated in R
	if (mb->jp) {
		static int first_time = 1;
		assert(!(mb->update));			       /* only one at the time... */
		evaluate_hyper_prior = 0;
		if (first_time) {
			// the first time only, set the R_HOME variable
			char *env = NULL;
			GMRFLib_sprintf(&env, "R_HOME=%s", mb->jp->R_HOME);
			my_setenv(env, 0);
			Free(env);
			// and source the Rfile
			inla_R_source(mb->jp->Rfile);
			first_time = 0;
		}

		int verbose = 0;
		double *lprior = NULL;
		int n_out;

		inla_R_funcall1(&n_out, &lprior, (const char *) mb->jp->func, ntheta, theta);
		assert(n_out == 1);
		assert(lprior);
		val += *lprior;
		if (verbose)
			printf("got lprior = %g\n", *lprior);
		Free(lprior);
	}

	/*
	 * this is for the linear predictor
	 */
	if (!mb->predictor_fixed) {
		log_precision = theta[count];
		count++;
	} else {
		log_precision = mb->predictor_log_prec[GMRFLib_thread_id][0];
	}
	val += mb->predictor_n * (LOG_NORMC_GAUSSIAN + 1.0 / 2.0 * log_precision);
	if (!mb->predictor_fixed) {
		val += PRIOR_EVAL(mb->predictor_prior, &log_precision);
	}

	/*
	 * this is for the A-matrix
	 */
	if (mb->predictor_m > 0) {
		log_precision = log(mb->predictor_Aext_precision);
		val += mb->predictor_m * (LOG_NORMC_GAUSSIAN + 1.0 / 2.0 * log_precision);
	}

	if (mb->data_ntheta_all) {
		int check = 0;

		for (j = 0; j < mb->nds; j++) {
			Data_section_tp *ds = &(mb->data_sections[j]);

			check += ds->data_ntheta;
			switch (ds->data_id) {
			case L_GAUSSIAN:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_SIMPLEX:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_GPOISSON:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_overdispersion = theta[count];
					val += PRIOR_EVAL(ds->data_prior0, &log_overdispersion);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double p = theta[count];
					val += PRIOR_EVAL(ds->data_prior1, &p);
					count++;
				}
				break;

			case L_CIRCULAR_NORMAL:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_WRAPPED_CAUCHY:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_TEST_BINOMIAL_1:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double s = theta[count];
					val += PRIOR_EVAL(ds->data_prior0, &s);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double e = theta[count];
					val += PRIOR_EVAL(ds->data_prior1, &e);
					count++;
				}
				break;

			case L_IID_GAMMA:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_shape = theta[count];
					val += PRIOR_EVAL(ds->data_prior0, &log_shape);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_rate = theta[count];
					val += PRIOR_EVAL(ds->data_prior1, &log_rate);
					count++;
				}
				break;

			case L_IID_LOGITBETA:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_a = theta[count];
					val += PRIOR_EVAL(ds->data_prior0, &log_a);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_b = theta[count];
					val += PRIOR_EVAL(ds->data_prior1, &log_b);
					count++;
				}
				break;

			case L_LOGGAMMA_FRAILTY:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_LOGNORMAL:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_LOGISTIC:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_SKEWNORMAL:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_precision);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the shape-parameter
					 */
					double shape = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &shape);
					count++;
				}
				break;

			case L_SKEWNORMAL2:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_precision);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the skewness
					 */
					double logit_skew = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &logit_skew);
					count++;
				}
				break;

			case L_GEV:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_precision);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the gev-parameter. Note that we need to scale it back to the real scale, as 'scale_xi' is there to help the
					 * numerics only
					 */
					double xi = theta[count] * ds->data_observations.gev_scale_xi;

					val += PRIOR_EVAL(ds->data_prior1, &xi);
					count++;
				}
				break;

			case L_GAMMA:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double precision_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &precision_intern);
					count++;
				}
				break;

			case L_GAMMACOUNT:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_alpha = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &log_alpha);
					count++;
				}
				break;

			case L_BETA:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double precision_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &precision_intern);
					count++;
				}
				break;
			case L_BETABINOMIAL:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double intern_overdispersion = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &intern_overdispersion);
					count++;
				}
				break;

			case L_NBINOMIAL:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &log_size);
					count++;
				}
				break;

			case L_ZEROINFLATEDNBINOMIAL0:
			case L_ZEROINFLATEDNBINOMIAL1:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_size);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the probability-parameter in the zero-inflated nbinomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &prob_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDNBINOMIAL2:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_size);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the alpha-parameter in the zero-inflated nbinomial_0/1
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &alpha_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDBETABINOMIAL0:
			case L_ZEROINFLATEDBETABINOMIAL1:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double rho_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &rho_intern);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the probability-parameter in the zero-inflated betabinomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &prob_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDNBINOMIAL1STRATA2:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_size);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the probability-parameter1 in the zero-inflated nbinomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &prob_intern);
					count++;
				}
				if (!ds->data_fixed2) {
					/*
					 * this is the probability-parameter2 in the zero-inflated nbinomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior2, &prob_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDNBINOMIAL1STRATA3:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_size);
					count++;
				}

				if (!ds->data_fixed1) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_size = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &log_size);
					count++;
				}
				if (!ds->data_fixed2) {
					/*
					 * this is the probability-parameter2 in the zero-inflated nbinomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior2, &prob_intern);
					count++;
				}
				break;

			case L_ZERO_N_INFLATEDBINOMIAL2:
				if (!ds->data_fixed0) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					double log_alpha1 = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &log_alpha1);
					count++;
				}
				if (!ds->data_fixed1) {
					double log_alpha2 = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &log_alpha2);
					count++;
				}
				break;

			case L_LAPLACE:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is included in the likelihood
					 * function.
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_T:
				/*
				 * we only need to add the prior, since the normalisation constant due to the likelihood, is
				 * included in the likelihood function. 
				 */
				if (!ds->data_fixed0) {
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior0, &log_precision);
					count++;
				}
				if (!ds->data_fixed1) {
					dof_intern = theta[count];
					val += PRIOR_EVAL(ds->data_prior1, &dof_intern);
					count++;
				}
				break;

			case L_TSTRATA:
			{
				/*
				 * we only need to add the prior, since the normalisation constant due to the likelihood, is
				 * included in the likelihood function. 
				 */
				int k;
				for (k = 0; k < TSTRATA_MAXTHETA; k++) {
					if (!ds->data_nfixed[k]) {
						double th = theta[count];
						val += PRIOR_EVAL(ds->data_nprior[k], &th);
						count++;
					}
				}
				break;
			}

			case L_STOCHVOL:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is
					 * included in the likelihood function. this is the offset precision
					 */
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &log_precision);
					count++;
				}
				break;

			case L_STOCHVOL_T:
				if (!ds->data_fixed) {
					/*
					 * we only need to add the prior, since the normalisation constant due to the likelihood, is
					 * included in the likelihood function. 
					 */
					dof_intern = theta[count];
					val += PRIOR_EVAL(ds->data_prior, &dof_intern);
					count++;
				}
				break;

			case L_STOCHVOL_NIG:
				if (!ds->data_fixed0) {
					/*
					 * this is the skewness 
					 */
					double skew = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &skew);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the shape 
					 */
					double shape_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &shape_intern);
					count++;
				}
				break;

			case L_WEIBULL:
				if (!ds->data_fixed) {
					/*
					 * this is the alpha-parameter in the Weibull 
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &alpha_intern);
					count++;
				}
				break;

			case L_LOGLOGISTIC:
				if (!ds->data_fixed) {
					/*
					 * this is the alpha-parameter in the LogLogistic
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &alpha_intern);
					count++;
				}
				break;

			case L_WEIBULL_CURE:
				if (!ds->data_fixed0) {
					/*
					 * this is the alpha-parameter in PS
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &alpha_intern);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the p-parameter in PS
					 */
					double p_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &p_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDPOISSON0:
			case L_ZEROINFLATEDPOISSON1:
				if (!ds->data_fixed) {
					/*
					 * this is the probability-parameter in the zero-inflated Poisson_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &prob_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDPOISSON2:
				if (!ds->data_fixed) {
					/*
					 * this is the probability-parameter in the zero-inflated Poisson_2
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &alpha_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDBINOMIAL2:
				if (!ds->data_fixed) {
					/*
					 * this is the probability-parameter in the zero-inflated Binomial_2
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &alpha_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDBINOMIAL0:
			case L_ZEROINFLATEDBINOMIAL1:
				if (!ds->data_fixed) {
					/*
					 * this is the probability-parameter in the zero-inflated Binomial_0/1
					 */
					double prob_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior, &prob_intern);
					count++;
				}
				break;

			case L_ZEROINFLATEDBETABINOMIAL2:
				if (!ds->data_fixed0) {
					/*
					 * this is the probability-related-parameter 
					 */
					double alpha_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior0, &alpha_intern);
					count++;
				}
				if (!ds->data_fixed1) {
					/*
					 * this is the delta-parameter 
					 */
					double delta_intern = theta[count];

					val += PRIOR_EVAL(ds->data_prior1, &delta_intern);
					count++;
				}
				break;

			default:
				/*
				 * nothing to do
				 */
				break;
			}

			/*
			 * link-models 
			 */
			check += ds->link_ntheta;

			if (debug) {
				P(ds->link_id);
				P(ds->link_ntheta);
			}

			switch (ds->link_id) {
			case LINK_IDENTITY:
			case LINK_LOG:
			case LINK_PROBIT:
			case LINK_CLOGLOG:
			case LINK_LOGIT:
			case LINK_TAN:
				break;

			case LINK_LOGOFFSET:
				if (!ds->link_fixed[0]) {
					double beta_intern = theta[count];
					val += PRIOR_EVAL(ds->link_prior[0], &beta_intern);
					count++;
				}
				break;

			case LINK_SSLOGIT:
				if (!ds->link_fixed[0]) {
					double sensitivity_intern = theta[count];
					val += PRIOR_EVAL(ds->link_prior[0], &sensitivity_intern);
					count++;
				}
				if (!ds->link_fixed[1]) {
					double specificity_intern = theta[count];
					val += PRIOR_EVAL(ds->link_prior[1], &specificity_intern);
					count++;
				}
				break;

			case LINK_TEST1:
				if (!ds->link_fixed[0]) {
					double beta = theta[count];
					val += PRIOR_EVAL(ds->link_prior[0], &beta);
					count++;
				}
				break;

			case LINK_SPECIAL2:
				if (!ds->link_fixed[0]) {
					double beta = theta[count];
					val += PRIOR_EVAL(ds->link_prior[0], &beta);
					count++;
				}
				break;

			case LINK_SPECIAL1:
				if (!ds->link_fixed[0]) {
					double log_precision;
					log_precision = theta[count];
					val += PRIOR_EVAL(ds->link_prior[0], &log_precision);
					count++;
				} else {
					// log_precision = ds->link_parameters->log_prec[GMRFLib_thread_id][0]; 
				}

				if (ds->link_order > 0) {
					double *beta = Calloc(ds->link_order, double);
					for (j = 0; j < ds->link_order; j++) {
						if (!ds->link_fixed[j + 1]) {
							beta[j] = theta[count];
							count++;
						} else {
							beta[j] = ds->link_parameters->betas[j][GMRFLib_thread_id][0];
						}
					}
					val += PRIOR_EVAL(ds->link_prior[1], beta);
					Free(beta);
				}
				break;

			default:
				assert(0 == 1);
			}

			/*
			 * re-models 
			 */
			if (ds->mix_use) {
				check += ds->mix_ntheta;

				switch (ds->mix_id) {
				case MIX_GAUSSIAN:
					if (!ds->mix_fixed) {
						log_precision = theta[count];
						val += PRIOR_EVAL(ds->mix_prior, &log_precision);
						count++;
					}
					break;

				default:
					assert(0 == 1);
				}
			}
		}
		assert(mb->data_ntheta_all == check);
	}

	for (i = 0; i < mb->nlinear; i++) {
		if (mb->linear_precision[i] > 0.0) {
			val += LOG_NORMC_GAUSSIAN + 1.0 / 2.0 * log(mb->linear_precision[i]);
		}
	}

	for (i = 0; i < mb->nf; i++) {
		switch (mb->f_id[i]) {
		case F_RW2D:
		case F_BESAG:
		case F_GENERIC0:
		case F_SEASONAL:
		case F_IID:
		case F_RW1:
		case F_RW2:
		case F_CRW2:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(1);

			double scale_correction = 0.0;
			if (mb->f_id[i] == F_IID && mb->f_scale[i]) {
				int ii, nii = mb->f_N[i] / mb->f_ngroup[i];

				for (ii = 0; ii < nii; ii++) {
					scale_correction += log(mb->f_scale[i][ii]);
				}
				scale_correction /= nii;
			}

			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i]) +
								   (mb->f_N[i] - mb->f_rankdef[i]) / 2.0 * (log_precision + scale_correction)));
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			break;
		}

		case F_SPDE:
		{
			int k, nT, nK, nt, Toffset = 0, Koffset = 0;
			double t;
			inla_spde_tp *spde;
			double *Tpar = NULL, *Kpar = NULL, init0, init1, init2, init3;

			spde = (inla_spde_tp *) mb->f_model[i];
			assert(spde->Qfunc_arg == spde);

			nT = spde->Tmodel->ntheta;
			nK = spde->Kmodel->ntheta;
			nt = mb->f_ntheta[i];

			fixed0 = !NOT_FIXED(f_fixed[i][0]);
			fixed1 = !NOT_FIXED(f_fixed[i][1]);
			fixed2 = !NOT_FIXED(f_fixed[i][2]);
			fixed3 = !NOT_FIXED(f_fixed[i][3]);
			init0 = mb->f_initial[i][0];
			init1 = mb->f_initial[i][1];
			init2 = mb->f_initial[i][2];
			init3 = mb->f_initial[i][3];

			Tpar = Calloc(nT, double);
			Kpar = Calloc(nK, double);

			if (debug) {
				P(i);
				P(mb->f_fixed[i][0]);
				P(mb->f_fixed[i][1]);
				P(mb->f_fixed[i][2]);
				P(mb->f_fixed[i][3]);
				P(nT);
				P(nK);
				P(nt);
				P(fixed0);
				P(fixed1);
				P(fixed2);
				P(fixed3);
				P(init0);
				P(init1);
				P(init2);
				P(init3);
			}

			if (nT) {
				if (fixed0) {
					Tpar[0] = init0;
				} else {
					Tpar[0] = theta[count];
					Toffset++;
				}
				if (nT > 1) {
					if (fixed2) {
						for (k = 1; k < nT; k++)
							Tpar[k] = init2;
					} else {
						for (k = 1; k < nT; k++) {
							Tpar[k] = theta[count + Toffset];
							Toffset++;
						}
					}
				}
				spde->Tmodel->theta_extra[GMRFLib_thread_id] = Tpar;
			}

			if (debug) {
				P(count);
				P(Toffset);
				for (k = 0; k < nT; k++) {
					printf("Tpar[%d] = %g\n", k, Tpar[k]);
				}
			}

			if (nK) {
				if (fixed1) {
					Kpar[0] = init1;
				} else {
					Kpar[0] = theta[count + Toffset];
					Koffset++;
				}
				if (nK > 1) {
					if (fixed2) {
						for (k = 1; k < nK; k++)
							Kpar[k] = init2;
					} else {
						for (k = 1; k < nK; k++) {
							Kpar[k] = theta[count + Koffset + Toffset];
							Koffset++;
						}
					}
				}
				spde->Kmodel->theta_extra[GMRFLib_thread_id] = Kpar;
			}

			if (debug) {
				P(count);
				P(Toffset);
				P(Koffset);
				for (k = 0; k < nT; k++) {
					printf("Tpar[%d] = %g\n", k, Tpar[k]);
				}
				for (k = 0; k < nK; k++) {
					printf("Kpar[%d] = %g\n", k, Kpar[k]);
				}
			}

			if (fixed3) {
				spde->oc[GMRFLib_thread_id][0] = init3;
			} else {
				spde->oc[GMRFLib_thread_id][0] = theta[count + Koffset + Toffset];
			}

			if (debug) {
				printf("call extra() with\n");
				for (k = 0; k < nT; k++) {
					printf("Tmodel %d %g\n", k, spde->Tmodel->theta_extra[GMRFLib_thread_id][k]);
				}
				for (k = 0; k < nK; k++) {
					printf("Kmodel %d %g\n", k, spde->Kmodel->theta_extra[GMRFLib_thread_id][k]);
				}
				printf("Oc %g\n", spde->oc[GMRFLib_thread_id][0]);
			}

			/*
			 * T 
			 */
			if (nT) {
				if (NOT_FIXED(f_fixed[i][0])) {
					t = theta[count];
					val += PRIOR_EVAL(mb->f_prior[i][0], &t);
					count++;
				}
				for (k = 1; k < nT; k++) {
					if (NOT_FIXED(f_fixed[i][2])) {
						t = theta[count];
						val += PRIOR_EVAL(mb->f_prior[i][2], &t);
						count++;
					}
				}
			}

			/*
			 * K 
			 */
			if (nK) {
				if (NOT_FIXED(f_fixed[i][1])) {
					t = theta[count];
					val += PRIOR_EVAL(mb->f_prior[i][1], &t);
					count++;
				}
				for (k = 1; k < nK; k++) {
					if (NOT_FIXED(f_fixed[i][2])) {
						t = theta[count];
						val += PRIOR_EVAL(mb->f_prior[i][2], &t);
						count++;
					}
				}
			}
			/*
			 * Ocillating coeff 
			 */
			if (!fixed3) {
				t = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][3], &t);
				count++;
			}

			if (debug) {
				P(nT);
				P(nK);
				P(mb->f_ntheta[i]);
			}
			assert(IMAX(4, nT + nK + 1) + (mb->f_ngroup[i] > 1 ? 1 : 0) == mb->f_ntheta[i]);

			SET_GROUP_RHO(IMAX(4, nT + nK + 1));

			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as its so close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(spde->graph->n, double);

			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL,
							      spde->graph, spde->Qfunc, spde->Qfunc_arg, NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);

					for (ii = 0; ii < spde->graph->n; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			if (nT) {
				spde->Tmodel->theta_extra[GMRFLib_thread_id] = NULL;
			}
			if (nK) {
				spde->Kmodel->theta_extra[GMRFLib_thread_id] = NULL;
			}

			Free(Tpar);
			Free(Kpar);
			break;
		}

		case F_SPDE2:
		{
			int k, spde2_ntheta;
			inla_spde2_tp *spde2;

			spde2 = (inla_spde2_tp *) mb->f_model[i];
			assert(spde2->Qfunc_arg == spde2);

			spde2->debug = 0;
			spde2_ntheta = spde2->ntheta;
			if (!mb->fixed_mode) {
				for (k = 0; k < spde2_ntheta; k++) {
					spde2->theta[k][GMRFLib_thread_id][0] = theta[count + k];
				}
			}
			int count_ref = count;

			if (!mb->fixed_mode) {
				count += spde2_ntheta;	       /* as SET_GROUP_RHO need 'count' */
			}
			SET_GROUP_RHO(spde2_ntheta);

			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as its so close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(spde2->graph->n, double);

			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL,
							      spde2->graph, spde2->Qfunc, spde2->Qfunc_arg, NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);

					for (ii = 0; ii < spde2->graph->n; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			/*
			 * this is the mvnormal prior...  'count_ref' is the 'first theta as this is a mutivariate prior.
			 */
			if (!mb->fixed_mode) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &theta[count_ref]);
			}
			break;
		}

		case F_SPDE3:
		{
			int k, spde3_ntheta;
			inla_spde3_tp *spde3;

			spde3 = (inla_spde3_tp *) mb->f_model[i];
			assert(spde3->Qfunc_arg == spde3);

			spde3->debug = 0;
			spde3_ntheta = spde3->ntheta;
			if (!mb->fixed_mode) {
				for (k = 0; k < spde3_ntheta; k++) {
					spde3->theta[k][GMRFLib_thread_id][0] = theta[count + k];
				}
			}
			int count_ref = count;

			if (!mb->fixed_mode) {
				count += spde3_ntheta;	       /* as SET_GROUP_RHO need 'count' */
			}
			SET_GROUP_RHO(spde3_ntheta);

			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as its so close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(spde3->graph->n, double);

			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL,
							      spde3->graph, spde3->Qfunc, spde3->Qfunc_arg, NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);

					for (ii = 0; ii < spde3->graph->n; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			/*
			 * this is the mvnormal prior...  'count_ref' is the 'first theta as this is a mutivariate prior.
			 */
			if (!mb->fixed_mode) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &theta[count_ref]);
			}
			break;
		}

		case F_AR:
		{
			double log_precision, *pacf, *pacf_intern;
			int p;

			p = mb->f_order[i];
			assert(mb->f_ntheta[i] == p + 1);

			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}

			pacf = Calloc(p, double);
			pacf_intern = Calloc(p, double);
			for (j = 0; j < p; j++) {
				if (NOT_FIXED(f_fixed[i][j + 1])) {
					pacf_intern[j] = theta[count];
					count++;
				} else {
					pacf_intern[j] = mb->f_theta[i][j + 1][GMRFLib_thread_id][0];
				}
				pacf[j] = ar_map_pacf(pacf_intern[j], MAP_FORWARD, NULL);
			}
			SET_GROUP_RHO(mb->f_ntheta[i]);

			int n_ar;
			double marginal_prec, conditional_prec, *marginal_Q, *param, *zero, ldens;

			marginal_Q = Calloc(ISQR(p), double);
			ar_marginal_distribution(p, pacf, &marginal_prec, marginal_Q);
			conditional_prec = exp(log_precision) / marginal_prec;

			param = Calloc(1 + p + ISQR(p), double);
			zero = Calloc(p, double);
			param[0] = p;
			for (j = 0; j < ISQR(p); j++) {
				param[1 + p + j] = marginal_Q[j] * exp(log_precision);
			}

			n_ar = mb->f_n[i] / mb->f_nrep[i];
			ldens = priorfunc_mvnorm(zero, param) + (n_ar - p - mb->f_rankdef[i]) * (-0.5 * log(2 * M_PI) + 0.5 * log(conditional_prec));
			val += mb->f_nrep[i] * (ldens * (ngroup - grankdef) + normc_g);

			/*
			 * this is the mvnormal prior...  'count_ref' is the 'first theta as this is a mutivariate prior.
			 */
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			val += PRIOR_EVAL(mb->f_prior[i][1], pacf_intern);

			Free(pacf);
			Free(pacf_intern);
			Free(marginal_Q);
			Free(param);
			Free(zero);

			break;
		}

		case F_GENERIC1:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				beta_intern = theta[count];
				count++;
			} else {
				beta_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			beta = map_probability(beta_intern, MAP_FORWARD, NULL);
			SET_GROUP_RHO(2);

			double logdet_Q = 0.0;
			inla_generic1_tp *a = (inla_generic1_tp *) mb->f_Qfunc_arg[i];
			for (j = 0; j < n_orig; j++) {
				logdet_Q += log(1.0 - beta * a->eigenvalues[j] / a->max_eigenvalue);
			}

			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_n[i] - mb->f_rankdef[i])
								   + (mb->f_n[i] - mb->f_rankdef[i]) / 2.0 * log_precision + ngroup * 0.5 * logdet_Q));
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &beta_intern);
			}
			break;
		}

		case F_GENERIC2:
		{
			/*
			 * OOPS: even though the parameters are (log_prec, h2_inter), the prior is defined on (log_prec, log_prec_unstruct), with the proper
			 * Jacobian added. 
			 */
			double h2;

			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				h2_intern = theta[count];
				count++;
			} else {
				h2_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			h2 = map_probability(h2_intern, MAP_FORWARD, NULL);
			SET_GROUP_RHO(2);

			double log_prec_unstruct = log(h2 / (1.0 - h2)) + log_precision;
			double n = (double) mb->f_n[i];

			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (n / 2.0 + (n - mb->f_rankdef[i]) / 2.0) +
								   +(n - mb->f_rankdef[i]) / 2.0 * log_precision + n / 2.0 * log_prec_unstruct));

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_prec_unstruct);
			}
			/*
			 * The Jacobian for the change of variables, is
			 * 
			 * | d log_prec_unstruct / d h2_intern | = 1, so no need to correct for the Jacobian from the change of variables. 
			 */
			break;
		}

		case F_Z:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(1);

			inla_z_arg_tp *arg = (inla_z_arg_tp *) mb->f_Qfunc_arg_orig[i];
			arg->log_prec[GMRFLib_thread_id][0] = log_precision;

			// Parts of this code is a copy from F_SPDE2
			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as it may be close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(arg->n * arg->m, double);

			assert(mb->f_graph_orig[i]->n == arg->n + arg->m);

			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL, mb->f_graph_orig[i], mb->f_Qfunc_orig[i],
							      mb->f_Qfunc_arg_orig[i], NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);

					/*
					 * only need to add for the z-part; the last m components.
					 */
					for (ii = arg->n; ii < arg->n + arg->m; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			break;
		}

		case F_GENERIC3:
		{
			int k, kk;
			inla_generic3_tp *a;

			a = Calloc(1, inla_generic3_tp);
			memcpy((void *) a, (void *) mb->f_Qfunc_arg_orig[i], sizeof(inla_generic3_tp));
			a->log_prec = Calloc(GENERIC3_MAXTHETA, double **);
			for (k = 0; k < GENERIC3_MAXTHETA; k++) {
				if (NOT_FIXED(f_fixed[i][k])) {
					HYPER_NEW(a->log_prec[k], theta[count]);
					count++;
				} else {
					HYPER_NEW(a->log_prec[k], mb->f_theta[i][k][GMRFLib_thread_id][0]);
				}
			}
			SET_GROUP_RHO(GENERIC3_MAXTHETA);

			// Parts of this code is a copy from F_SPDE2
			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as it may be close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(a->n, double);

			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL,
							      mb->f_graph_orig[i],
							      mb->f_Qfunc_orig[i],
							      (void *) a, NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);

					/*
					 * only need to add for the z-part; the last m components.
					 */
					for (ii = 0; ii < a->n; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			for (k = 0; k < GENERIC3_MAXTHETA; k++) {
				if (NOT_FIXED(f_fixed[i][k])) {
					log_precision = a->log_prec[k][0][0];
					val += PRIOR_EVAL(mb->f_prior[i][k], &log_precision);
				}
			}

			/*
			 * Free 'a' 
			 */
			for (k = 0; k < GENERIC3_MAXTHETA; k++) {
				for (kk = 0; kk < GMRFLib_MAX_THREADS; kk++) {
					Free(a->log_prec[k][kk]);
				}
				Free(a->log_prec[k]);
			}
			Free(a->log_prec);
			Free(a);

			break;
		}


		case F_SLM:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				logit_rho = theta[count];
				count++;
			} else {
				logit_rho = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(2);

			inla_slm_arg_tp *arg = (inla_slm_arg_tp *) mb->f_Qfunc_arg_orig[i];
			arg->log_prec[GMRFLib_thread_id][0] = log_precision;
			arg->logit_rho[GMRFLib_thread_id][0] = logit_rho;

			// Parts of this code is a copy from F_SPDE2
			static GMRFLib_problem_tp **problem = NULL;
#pragma omp threadprivate(problem)

			if (problem == NULL) {
#pragma omp critical
				{
					if (problem == NULL) {
						problem = Calloc(mb->nf, GMRFLib_problem_tp *);
					}
				}
			}

			/*
			 * do a check for numerical not pos def matrix here, as it may be close to being singular 
			 */
			int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
			GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();
			double *cc_add = Calloc(arg->n + arg->m, double);

			assert(mb->f_graph_orig[i]->n == arg->n + arg->m);
			while (!ok) {
				retval = GMRFLib_init_problem(&problem[i], NULL, NULL, cc_add, NULL,
							      mb->f_graph_orig[i],
							      mb->f_Qfunc_orig[i],
							      mb->f_Qfunc_arg_orig[i], NULL, mb->f_constr_orig[i],
							      (problem[i] == NULL ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
				switch (retval) {
				case GMRFLib_EPOSDEF:
				{
					int ii;
					double eps = GMRFLib_eps(0.75);
					for (ii = 0; ii < mb->f_graph_orig[i]->n; ii++) {
						cc_add[ii] = (cc_add[ii] == 0.0 ? eps : cc_add[ii] * 10.0);
					}

					/*
					 * possible memory leak here, by purpose. if it fail, the internal structure might be incomplete and unsafe to free.
					 */
					problem[i] = NULL;
					break;
				}

				case GMRFLib_SUCCESS:
					ok = 1;
					break;

				default:
					/*
					 * some other error 
					 */
					GMRFLib_set_error_handler(old_handler);
					GMRFLib_ERROR(retval);
					abort();
					break;
				}

				if (++num_try >= num_try_max) {
					FIXME("This should not happen. Contact developers...");
					abort();
				}
			}
			Free(cc_add);
			GMRFLib_set_error_handler(old_handler);

			GMRFLib_evaluate(problem[i]);
			val += mb->f_nrep[i] * (problem[i]->sub_logdens * (ngroup - grankdef) + normc_g);

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &logit_rho);
			}
			break;
		}

		case F_MEC:
		{
			double mean_x, log_precision_x, log_precision_obs, beta_intern;

			if (NOT_FIXED(f_fixed[i][0])) {
				beta_intern = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][0], &beta_intern);
				count++;
			} else {
				beta_intern = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			beta = mb->f_theta_map[i][0] (beta_intern, MAP_FORWARD, mb->f_theta_map_arg[i][0]);

			if (NOT_FIXED(f_fixed[i][1])) {
				log_precision_obs = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_precision_obs);
				count++;
			} else {
				log_precision_obs = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}

			if (NOT_FIXED(f_fixed[i][2])) {
				mean_x = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][2], &mean_x);
				count++;
			} else {
				mean_x = mb->f_theta[i][2][GMRFLib_thread_id][0];
			}

			if (NOT_FIXED(f_fixed[i][3])) {
				log_precision_x = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][3], &log_precision_x);
				count++;
			} else {
				log_precision_x = mb->f_theta[i][3][GMRFLib_thread_id][0];
			}

			SET_GROUP_RHO(4);

			double AA = 0.0, BB = 0.0, CC = 0.0;
			int ii, nii = mb->f_N[i] / mb->f_ngroup[i];

			/*
			 * we need to do it like this as the scale[i] varies with 'i'.
			 */
			assert(mb->f_scale[i]);
			for (ii = 0; ii < nii; ii++) {
				AA += log((exp(log_precision_obs) * mb->f_scale[i][ii] + exp(log_precision_x)) / SQR(beta));
				BB += log(1.0 / (1.0 / (mb->f_scale[i][ii] * exp(log_precision_obs)) + 1.0 / exp(log_precision_x)));
				CC += SQR(mb->f_locations[i][ii] - mean_x) *
				    (1.0 / (1.0 / (mb->f_scale[i][ii] * exp(log_precision_obs)) + 1.0 / exp(log_precision_x)));
			}
			val += mb->f_nrep[i] * (normc_g + gcorr * (2.0 * LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i])
								   + mb->f_ngroup[i] * 0.5 * (AA + BB)
								   /*
								    * and the exponent which depends on the precisions. we need to correct with ngroup here as its
								    * not scaled with f_N that already is corrected for ngroup.
								    */
								   - mb->f_ngroup[i] * 0.5 * CC));
			break;
		}

		case F_MEB:
		{
			double beta_intern;

			if (NOT_FIXED(f_fixed[i][0])) {
				beta_intern = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][0], &beta_intern);
				count++;
			} else {
				beta_intern = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			beta = mb->f_theta_map[i][0] (beta_intern, MAP_FORWARD, mb->f_theta_map_arg[i][0]);

			if (NOT_FIXED(f_fixed[i][1])) {
				log_precision = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_precision);
				count++;
			} else {
				log_precision = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}

			SET_GROUP_RHO(2);

			double scale_correction = 0.0;
			int ii, nii = mb->f_N[i] / mb->f_ngroup[i];

			/*
			 * we need to do it like this as the scale[i] varies with 'i'.
			 */
			if (mb->f_scale[i]) {
				for (ii = 0; ii < nii; ii++) {
					scale_correction += log(mb->f_scale[i][ii]);
				}
				scale_correction /= nii;
			}
			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i]) +
								   (mb->f_N[i] - mb->f_rankdef[i]) / 2.0 * (log_precision + scale_correction - log(SQR(beta)))));
			break;
		}

		case F_R_GENERIC:
		{
#if !defined(WINDOWS)
			int ntheta = mb->f_ntheta[i], ii;
			inla_rgeneric_tp *def;
			double *param, log_norm_const, log_prior;

			def = (inla_rgeneric_tp *) mb->f_Qfunc_arg[i];
			param = Calloc(ntheta, double);
			for (ii = 0; ii < ntheta; ii++) {
				param[ii] = theta[count];
				count++;
			}
#pragma omp critical
			{
				WRITE_MSG(def->c2R, def->Id, R_GENERIC_LOG_NORM_CONST);
				WRITE(def->c2R, param, ntheta, double);
				READ(def->R2c, &log_norm_const, 1, double);
				WRITE_MSG(def->c2R, def->Id, R_GENERIC_LOG_PRIOR);
				WRITE(def->c2R, param, ntheta, double);
				READ(def->R2c, &log_prior, 1, double);
			}

			SET_GROUP_RHO(ntheta);
			val += mb->f_nrep[i] * (normc_g + log_norm_const * (mb->f_ngroup[i] - grankdef)) + log_prior;
			Free(param);
#endif							       /* !defined(WINDOWS) */
			break;
		}

		case F_AR1:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				phi_intern = theta[count];
				count++;
			} else {
				phi_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			phi = map_phi(phi_intern, MAP_FORWARD, NULL);
			SET_GROUP_RHO(2);

			double log_precision_noise = log_precision - log(1.0 - SQR(phi));

			if (mb->f_cyclic[i]) {
				logdet = inla_ar1_cyclic_logdet(N_orig, phi);
				val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i])
									   + (mb->f_N[i] - mb->f_rankdef[i]) / 2.0 * log_precision_noise + ngroup * 0.5 * logdet));
			} else {
				val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i])
									   + (mb->f_N[i] - mb->f_rankdef[i]) / 2.0 * log_precision_noise
									   + ngroup * 0.5 * log(1.0 - SQR(phi))));
			}
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &phi_intern);
			}
			break;
		}

		case F_OU:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				phi_intern = theta[count];
				count++;
			} else {
				phi_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			phi = map_exp(phi_intern, MAP_FORWARD, NULL);
			SET_GROUP_RHO(2);

			int ii;
			double ou_nc = 0.0;
			int nn = ((inla_ou_arg_tp *) mb->f_Qfunc_arg_orig[i])->n;
			for (ii = 1; ii < nn; ii++) {
				ou_nc -= log(1.0 - exp(-2.0 * phi * (mb->f_locations[i][ii] - mb->f_locations[i][ii - 1])));
			}
			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] - mb->f_rankdef[i])
								   + (mb->f_N[i] - mb->f_rankdef[i]) / 2.0 * log_precision + ngroup * ou_nc / 2.0));

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &phi_intern);
			}
			break;
		}

		case F_BESAG2:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				a_intern = theta[count];
				count++;
			} else {
				a_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(2);
			// N is 2*graph->n here. 
			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (mb->f_N[i] / 2.0 - mb->f_rankdef[i])
								   + (mb->f_N[i] / 2.0 - mb->f_rankdef[i]) / 2.0 * (log_precision - 2.0 * a_intern)));
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &a_intern);
			}
			break;
		}

		case F_BYM:
		{
			if (NOT_FIXED(f_fixed[i][0])) {	       /* iid */
				log_precision0 = theta[count];
				count++;
			} else {
				log_precision0 = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {	       /* spatial */
				log_precision1 = theta[count];
				count++;
			} else {
				log_precision1 = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(2);

			double n = (double) mb->f_n[i];
			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * (n / 2.0 + (n - mb->f_rankdef[i]) / 2.0)
								   + n / 2.0 * log_precision0	/* iid */
								   + (n - mb->f_rankdef[i]) / 2.0 * log_precision1));	/* spatial */
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision0);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_precision1);
			}
			break;
		}

		case F_RW2DIID:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				phi_intern = theta[count];
				count++;
			} else {
				phi_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(2);

			double n = (double) mb->f_n[i];
			double phi = map_probability(phi_intern, MAP_FORWARD, NULL);

			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * n + n / 2.0 * (log_precision - log(1.0 - phi))));

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &phi_intern);
			}
			break;
		}


		case F_BYM2:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision = theta[count];
				count++;
			} else {
				log_precision = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				phi_intern = theta[count];
				count++;
			} else {
				phi_intern = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			SET_GROUP_RHO(2);

			double n = (double) mb->f_n[i];
			double phi = map_probability(phi_intern, MAP_FORWARD, NULL);

			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * n + n / 2.0 * (log_precision - log(1.0 - phi))));

			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &phi_intern);
			}
			break;
		}

		case F_2DIID:
		{
			if (mb->f_ngroup[i] > 1) {
				fprintf(stderr, "\n\n F_2DIID is not yet prepared for ngroup > 1\n");
				exit(EXIT_FAILURE);
			}

			assert(mb->f_ntheta[i] == 3);	       /* yes */
			if (NOT_FIXED(f_fixed[i][0])) {
				log_precision0 = theta[count];
				count++;
			} else {
				log_precision0 = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				log_precision1 = theta[count];
				count++;
			} else {
				log_precision1 = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][2])) {
				rho_intern = theta[count];
				count++;
			} else {
				rho_intern = mb->f_theta[i][2][GMRFLib_thread_id][0];
			}
			rho = map_rho(rho_intern, MAP_FORWARD, NULL);
			double n = (double) mb->f_n[i];
			assert(mb->f_ngroup[i] == 1);
			val += mb->f_nrep[i] * (LOG_NORMC_GAUSSIAN * 2.0 * (n - mb->f_rankdef[i])	/* yes, the total length is N=2n */
						+(n - mb->f_rankdef[i]) / 2.0 * log_precision0	/* and there is n-pairs... */
						+ (n - mb->f_rankdef[i]) / 2.0 * log_precision1 - (n - mb->f_rankdef[i]) / 2.0 * log(1.0 - SQR(rho)));
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &log_precision0);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_precision1);
			}
			if (NOT_FIXED(f_fixed[i][2])) {
				val += PRIOR_EVAL(mb->f_prior[i][2], &rho_intern);
			}
			break;
		}

		case F_IID1D:
		case F_IID2D:
		case F_IID3D:
		case F_IID4D:
		case F_IID5D:
		{
			int jj, count_ref = count;
			int dim = WISHART_DIM(i);
			assert(dim > 0);

			int nt = inla_iid_wishart_nparam(dim);
			double log_jacobian = 0.0;
			double *theta_vec = Calloc(nt, double);
			int k = 0;
			nfixed = 0;
			for (j = 0; j < dim; j++) {
				if (NOT_FIXED(f_fixed[i][k])) {
					theta_vec[k] = theta[count];
					count++;
				} else {
					nfixed++;
					theta_vec[k] = mb->f_theta[i][k][GMRFLib_thread_id][0];
				}
				log_jacobian += log(map_precision(theta_vec[k], MAP_DFORWARD, NULL));
				theta_vec[k] = map_precision(theta_vec[k], MAP_FORWARD, NULL);
				k++;
			}

			for (j = 0; j < dim; j++) {
				for (jj = j + 1; jj < dim; jj++) {
					if (NOT_FIXED(f_fixed[i][k])) {
						theta_vec[k] = theta[count];
						count++;
					} else {
						nfixed++;
						theta_vec[k] = mb->f_theta[i][k][GMRFLib_thread_id][0];
					}
					log_jacobian += log(map_rho(theta_vec[k], MAP_DFORWARD, NULL));
					theta_vec[k] = map_rho(theta_vec[k], MAP_FORWARD, NULL);
					k++;
				}
			}
			assert(k == nt);

			fail = inla_iid_wishart_adjust(dim, theta_vec);
			Q = gsl_matrix_calloc(dim, dim);
			k = 0;
			for (j = 0; j < dim; j++) {
				gsl_matrix_set(Q, j, j, 1.0 / theta_vec[k]);
				k++;
			}
			for (j = 0; j < dim; j++) {
				for (jj = j + 1; jj < dim; jj++) {
					double value = theta_vec[k] / sqrt(theta_vec[j] * theta_vec[jj]);
					gsl_matrix_set(Q, j, jj, value);
					gsl_matrix_set(Q, jj, j, value);
					k++;
				}
			}
			assert(k == nt);

			GMRFLib_gsl_spd_inverse(Q);
			logdet = GMRFLib_gsl_spd_logdet(Q);
			gsl_matrix_free(Q);

			SET_GROUP_RHO(nt);

			/*
			 * n is the small length 
			 */
			double n = (double) (mb->f_n[i] / dim);	/* YES! */
			val += mb->f_nrep[i] * (normc_g + gcorr * (LOG_NORMC_GAUSSIAN * dim * (n - mb->f_rankdef[i])	/* yes, the total length is N=dim*n */
								   +(n - mb->f_rankdef[i]) / 2.0 * logdet));
			if (fail) {
				val += PENALTY;
			}

			/*
			 * if all parameters are fixed, there is no prior to add
			 */
			if (count - count_ref > 0) {
				if (nfixed) {
					static char first = 1;
					if (first) {
						fprintf(stderr,
							"\n\n\nWARNING: Wishart prior is not corrected to account for %d fixed hyperparameters.\n\n", nfixed);
						first = 0;
					}
				}
				/*
				 * prior density wrt theta. Include here the Jacobian from going from (precision0, precision1, rho), to theta = (log_precision0,
				 * log_precision1, rho_intern). 
				 */
				val += PRIOR_EVAL(mb->f_prior[i][0], theta_vec) + log_jacobian;
			}
			break;
		}

		case F_MATERN2D:
		{
			/*
			 * this is the safe version, which also works correctly for diagonal > 0. It is possible to reuse the calculations for the same
			 * range and different precision, provided diagonal = 0, but care must be taken about the constraints.
			 */

			typedef struct {
				int n;
				int N;
				int ngroup;
				int nrep;
				double precision;
				double range;
				double *c;
				double rankdef1;
				GMRFLib_matern2ddef_tp *matern2ddef;
				GMRFLib_problem_tp *problem;
			} Hold_tp;
			static Hold_tp **hold = NULL;
#pragma omp threadprivate(hold)
			int jj;
			Hold_tp *h;
			GMRFLib_matern2ddef_tp *q;

			if (!hold) {
				hold = Calloc(mb->nf, Hold_tp *);
			}

			if (!hold[i]) {
				h = hold[i] = Calloc(1, Hold_tp);

				h->nrep = mb->f_nrep[i];
				h->ngroup = mb->f_ngroup[i];
				h->n = mb->f_n[i] / h->ngroup;
				h->N = mb->f_N[i] / h->ngroup;

				assert(h->N == mb->f_graph_orig[i]->n);

				if (debug) {
					P(h->n);
					P(h->N);
					P(h->nrep);
					P(h->ngroup);
				}

				if (mb->f_diag[i]) {
					h->c = Calloc(h->N, double);
					for (jj = 0; jj < h->N; jj++) {
						h->c[jj] = mb->f_diag[i];
					}
				}

				q = (GMRFLib_matern2ddef_tp *) mb->f_Qfunc_arg_orig[i];
				if (debug) {
					P(q->nrow);
					P(q->ncol);
					P(q->cyclic);
					P(q->nu);
				}

				h->matern2ddef = Calloc(1, GMRFLib_matern2ddef_tp);
				memcpy(h->matern2ddef, q, sizeof(GMRFLib_matern2ddef_tp));
				h->matern2ddef->prec = &h->precision;
				h->matern2ddef->range = &h->range;

				h->matern2ddef->log_prec = NULL;
				h->matern2ddef->log_prec_omp = NULL;
				h->matern2ddef->log_range = NULL;
				h->matern2ddef->log_range_omp = NULL;
			} else {
				h = hold[i];
			}

			if (NOT_FIXED(f_fixed[i][0])) {
				h->precision = map_precision(theta[count], MAP_FORWARD, NULL);
				val += PRIOR_EVAL(mb->f_prior[i][0], &theta[count]);
				count++;
			} else {
				h->precision = map_precision(mb->f_theta[i][0][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				h->range = map_range(theta[count], MAP_FORWARD, NULL);
				val += PRIOR_EVAL(mb->f_prior[i][1], &theta[count]);
				count++;
			} else {
				h->range = map_range(mb->f_theta[i][1][GMRFLib_thread_id][0], MAP_FORWARD, NULL);
			}

			SET_GROUP_RHO(2);

			GMRFLib_init_problem(&(h->problem), NULL, NULL, h->c, NULL, mb->f_graph_orig[i], mb->f_Qfunc_orig[i],
					     (void *) h->matern2ddef, NULL, mb->f_constr_orig[i],
					     (!h->problem ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
			if (debug) {
				P(h->precision);
				P(h->range);
				P(h->problem->sub_logdens);
			}
			val += h->nrep * (h->problem->sub_logdens * (ngroup - grankdef) + normc_g);
			break;
		}

		case F_BESAGPROPER:
		{
			typedef struct {
				int n;
				int N;
				int ngroup;
				int nrep;
				double **log_prec;
				double **log_diag;
				double *c;
				double rankdef1;
				inla_besag_proper_Qfunc_arg_tp *def;
				GMRFLib_problem_tp *problem;
			} Hold_tp;
			static Hold_tp **hold = NULL;
#pragma omp threadprivate(hold)

			int jj;
			Hold_tp *h;

			if (!hold) {
				hold = Calloc(mb->nf, Hold_tp *);
			}

			if (!hold[i]) {
				h = hold[i] = Calloc(1, Hold_tp);

				h->nrep = mb->f_nrep[i];
				h->ngroup = mb->f_ngroup[i];
				h->n = mb->f_n[i] / h->ngroup;
				h->N = mb->f_N[i] / h->ngroup;

				assert(h->N == mb->f_graph_orig[i]->n);

				if (debug) {
					P(h->n);
					P(h->N);
					P(h->nrep);
					P(h->ngroup);
				}

				HYPER_NEW(h->log_prec, 0.0);
				HYPER_NEW(h->log_diag, 0.0);

				if (mb->f_diag[i]) {
					h->c = Calloc(h->N, double);
					for (jj = 0; jj < h->N; jj++) {
						h->c[jj] = mb->f_diag[i];
					}
				}

				h->def = Calloc(1, inla_besag_proper_Qfunc_arg_tp);
				memcpy(h->def, mb->f_Qfunc_arg_orig[i], sizeof(inla_besag_proper_Qfunc_arg_tp));
				h->def->log_prec = h->log_prec;
				h->def->log_diag = h->log_diag;
			} else {
				h = hold[i];
			}

			if (NOT_FIXED(f_fixed[i][0])) {
				h->log_prec[GMRFLib_thread_id][0] = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][0], &theta[count]);
				count++;
			} else {
				h->log_prec[GMRFLib_thread_id][0] = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				h->log_diag[GMRFLib_thread_id][0] = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][1], &theta[count]);
				count++;
			} else {
				h->log_diag[GMRFLib_thread_id][0] = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}

			SET_GROUP_RHO(2);

			GMRFLib_init_problem(&(h->problem), NULL, NULL, h->c, NULL, mb->f_graph_orig[i], mb->f_Qfunc_orig[i],
					     (void *) h->def, NULL, mb->f_constr_orig[i],
					     (!h->problem ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));
			if (debug) {
				P(h->log_prec[GMRFLib_thread_id][0]);
				P(h->log_diag[GMRFLib_thread_id][0]);
				P(h->problem->sub_logdens);
			}
			val += h->nrep * (h->problem->sub_logdens * (ngroup - grankdef) + normc_g);
			break;
		}

		case F_BESAGPROPER2:
		{
			typedef struct {
				int n;
				int N;
				int ngroup;
				int nrep;
				double **log_prec;
				double **logit_lambda;
				double *c;
				double rankdef1;
				inla_besag_proper2_Qfunc_arg_tp *def;
				GMRFLib_problem_tp *problem;
			} Hold_tp;
			static Hold_tp **hold = NULL;
#pragma omp threadprivate(hold)

			int jj;
			Hold_tp *h;

			if (!hold) {
				hold = Calloc(mb->nf, Hold_tp *);
			}

			if (!hold[i]) {
				h = hold[i] = Calloc(1, Hold_tp);

				h->nrep = mb->f_nrep[i];
				h->ngroup = mb->f_ngroup[i];
				h->n = mb->f_n[i] / h->ngroup;
				h->N = mb->f_N[i] / h->ngroup;

				assert(h->N == mb->f_graph_orig[i]->n);

				if (debug) {
					P(h->n);
					P(h->N);
					P(h->nrep);
					P(h->ngroup);
				}

				HYPER_NEW(h->log_prec, 0.0);
				HYPER_NEW(h->logit_lambda, 0.0);

				if (mb->f_diag[i]) {
					h->c = Calloc(h->N, double);
					for (jj = 0; jj < h->N; jj++) {
						h->c[jj] = mb->f_diag[i];
					}
				}

				h->def = Calloc(1, inla_besag_proper2_Qfunc_arg_tp);
				memcpy(h->def, mb->f_Qfunc_arg_orig[i], sizeof(inla_besag_proper2_Qfunc_arg_tp));
				h->def->log_prec = h->log_prec;
				h->def->logit_lambda = h->logit_lambda;
			} else {
				h = hold[i];
			}

			if (NOT_FIXED(f_fixed[i][0])) {
				h->log_prec[GMRFLib_thread_id][0] = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][0], &theta[count]);
				count++;
			} else {
				h->log_prec[GMRFLib_thread_id][0] = mb->f_theta[i][0][GMRFLib_thread_id][0];
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				h->logit_lambda[GMRFLib_thread_id][0] = theta[count];
				val += PRIOR_EVAL(mb->f_prior[i][1], &theta[count]);
				count++;
			} else {
				h->logit_lambda[GMRFLib_thread_id][0] = mb->f_theta[i][1][GMRFLib_thread_id][0];
			}

			SET_GROUP_RHO(2);

			GMRFLib_init_problem(&(h->problem), NULL, NULL, h->c, NULL, mb->f_graph_orig[i], mb->f_Qfunc_orig[i],
					     (void *) h->def, NULL, mb->f_constr_orig[i],
					     (!h->problem ? GMRFLib_NEW_PROBLEM : GMRFLib_KEEP_graph | GMRFLib_KEEP_mean));

			val += h->nrep * (h->problem->sub_logdens * (ngroup - grankdef) + normc_g);
			break;
		}

		case F_COPY:
		{
			if (NOT_FIXED(f_fixed[i][0]) && !mb->f_same_as[i]) {
				beta = theta[count];
				count++;
			} else {
				/*
				 * not needed 
				 */
				// if (mb->f_theta[i]) {
				// beta = mb->f_theta[i][0][GMRFLib_thread_id][0];
				// }
			}
			if (NOT_FIXED(f_fixed[i][0]) && !mb->f_same_as[i]) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &beta);
			}
			break;
		}

		case F_CLINEAR:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				beta = theta[count];
				count++;
			} else {
				// 
			}
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &beta);
			}
			break;
		}

		case F_SIGM:
		case F_REVSIGM:
		{
			if (NOT_FIXED(f_fixed[i][0])) {
				beta = theta[count];
				count++;
			} else {
				// 
			}
			if (NOT_FIXED(f_fixed[i][0])) {
				val += PRIOR_EVAL(mb->f_prior[i][0], &beta);
			}

			if (NOT_FIXED(f_fixed[i][1])) {
				log_halflife = theta[count];
				count++;
			} else {
				// 
			}
			if (NOT_FIXED(f_fixed[i][1])) {
				val += PRIOR_EVAL(mb->f_prior[i][1], &log_halflife);
			}

			if (NOT_FIXED(f_fixed[i][2])) {
				log_shape = theta[count];
				count++;
			} else {
				// 
			}
			if (NOT_FIXED(f_fixed[i][2])) {
				val += PRIOR_EVAL(mb->f_prior[i][2], &log_shape);
			}
			break;
		}

		default:
			P(mb->f_id[i]);
			abort();
			assert(0 == 1);
			break;
		}
	}

	if (debug) {
		P(count);
		P(mb->ntheta);
		P(ntheta);
	}

	assert((count == mb->ntheta) && (count == ntheta));    /* check... */
#undef SET_GROUP_RHO
#undef NOT_FIXED
	return val;
}
double inla_compute_initial_value(int idx, GMRFLib_logl_tp * loglfunc, double *x_vec, void *arg)
{
	/*
	 * solve arg min logl(x[i]) - prec * 0.5*x[i]^2. But we have no option of what PREC is, so I set it to 1.0
	 */
	double prec = 1.0, x, xnew, f, deriv, dderiv, arr[3], xarr[3], eps = 1.0e-4;
	int niter = 0, compute_deriv, retval, niter_max = 10, debug = 0;

	GMRFLib_thread_id = 0;				       /* yes, this is what we want! */
	x = xnew = 0.0;

	retval = loglfunc(NULL, NULL, 0, 0, NULL, NULL);
	compute_deriv = (retval == GMRFLib_LOGL_COMPUTE_DERIVATIES || retval == GMRFLib_LOGL_COMPUTE_DERIVATIES_AND_CDF);

	while (1) {

		if (compute_deriv) {
			xarr[0] = xarr[1] = xarr[2] = x;
			loglfunc(arr, xarr, 3, idx, x_vec, arg);
		} else {
			GMRFLib_2order_taylor(&arr[0], &arr[1], &arr[2], 1.0, x, idx, x_vec, loglfunc, arg, NULL);
		}
		f = arr[0] - 0.5 * prec * SQR(x);
		deriv = arr[1] - prec * x;
		dderiv = DMIN(0.0, arr[2]) - prec;

		xnew = x - DMIN(0.25 + niter * 0.25, 1.0) * deriv / dderiv;
		if (debug) {
			printf("idx %d x %.10g xnew %.10g f %.10g deriv %.10g dderiv %.10g\n", idx, x, xnew, f, deriv, dderiv);
		}
		x = xnew;

		if (ABS(deriv / dderiv) < eps) {
			break;
		}
		if (++niter > niter_max) {
			x = 0.0;
			break;
		}
	}

	return x;
}
int inla_INLA(inla_tp * mb)
{
	double *c = NULL, *x = NULL, *b = NULL;
	int N, i, j, k, count;
	char *compute = NULL;
	GMRFLib_bfunc_tp **bfunc;

	if (mb->verbose) {
		printf("%s...\n", __GMRFLib_FuncName);
	}

	/*
	 * We need to determine the strategy if strategy is default 
	 */
	if (mb->strategy == GMRFLib_OPENMP_STRATEGY_DEFAULT) {
		/*
		 * to determine the strategy, count the size of the model 
		 */
		int ntot = 0;

		ntot += mb->predictor_n + mb->predictor_m + mb->nlinear;
		for (i = 0; i < mb->nf; i++)
			ntot += mb->f_graph[i]->n;
		if (mb->verbose) {
			printf("\tStrategy = [DEFAULT]\n");

			char *sname;
			if (ntot < 500) {
				mb->strategy = GMRFLib_OPENMP_STRATEGY_SMALL;
				sname = GMRFLib_strdup("SMALL");
			} else if (ntot < 2000) {
				mb->strategy = GMRFLib_OPENMP_STRATEGY_MEDIUM;
				sname = GMRFLib_strdup("MEDIUM");
			} else if (ntot < 50000) {
				mb->strategy = GMRFLib_OPENMP_STRATEGY_LARGE;
				sname = GMRFLib_strdup("LARGE");
			} else {
				mb->strategy = GMRFLib_OPENMP_STRATEGY_HUGE;
				sname = GMRFLib_strdup("HUGE");
			}
			printf("\tSize is [%1d] and strategy [%s] is chosen\n", ntot, sname);
		}
	}
	GMRFLib_openmp->strategy = mb->strategy;

	GMRFLib_init_hgmrfm(&(mb->hgmrfm), mb->predictor_n, mb->predictor_m,
			    mb->predictor_cross_sumzero, NULL, mb->predictor_log_prec,
			    (const char *) mb->predictor_Aext_fnm, mb->predictor_Aext_precision,
			    mb->nf, mb->f_c, mb->f_weights, mb->f_graph, mb->f_Qfunc, mb->f_Qfunc_arg, mb->f_sumzero, mb->f_constr,
			    mb->ff_Qfunc, mb->ff_Qfunc_arg, mb->nlinear, mb->linear_covariate, mb->linear_precision,
			    (mb->lc_derived_only ? 0 : mb->nlc), mb->lc_lc, mb->lc_prec, mb->ai_par);
	N = ((GMRFLib_hgmrfm_arg_tp *) mb->hgmrfm->Qfunc_arg)->N;
	if (mb->verbose) {
		printf("\tSize of graph=[%1d] constraints=[%1d]\n", N, (mb->hgmrfm->constr ? mb->hgmrfm->constr->nc : 0));
	}

	mb->d = Realloc(mb->d, N, double);
	memset(&(mb->d[mb->predictor_ndata]), 0, (N - mb->predictor_ndata) * sizeof(double));
	mb->loglikelihood = Realloc(mb->loglikelihood, N, GMRFLib_logl_tp *);
	memset(&(mb->loglikelihood[mb->predictor_ndata]), 0, (N - mb->predictor_ndata) * sizeof(GMRFLib_logl_tp *));
	mb->loglikelihood_arg = Realloc(mb->loglikelihood_arg, N, void *);
	memset(&(mb->loglikelihood_arg[mb->predictor_ndata]), 0, (N - mb->predictor_ndata) * sizeof(void *));

	if (0) {
		for (i = 0; i < N; i++)
			printf("d[%d]=%g\n", i, mb->d[i]);
	}

	/*
	 * add the diagonal, if any 
	 */
	c = Calloc(N, double);
	count = mb->predictor_n + mb->predictor_m;
	for (i = 0; i < mb->nf; i++) {
		for (k = 0; k < mb->f_nrep[i]; k++) {
			for (j = 0; j < mb->f_n[i]; j++) {
				c[count + j + k * mb->f_N[i]] = mb->f_diag[i];	/* yes; this is correct */
			}
		}
		count += mb->f_Ntotal[i];		       /* yes; this is correct */
	}

	/*
	 * this is an emergency option to prevent singular matrices (and is known to be >= 0) 
	 */
	if (mb->expert_diagonal_emergencey) {
		for (i = mb->predictor_n + mb->predictor_m; i < N; i++)
			c[i] += mb->expert_diagonal_emergencey;
	}

	if (0) {
		for (i = 0; i < N; i++)
			printf("c[%d]=%g\n", i, c[i]);
	}

	/*
	 * mark those we want to compute  and compute the b
	 */
	compute = Calloc(N, char);
	b = Calloc(N, double);
	bfunc = Calloc(N, GMRFLib_bfunc_tp *);
	count = 0;
	if (mb->expert_cpo_manual) {
		/*
		 * if set, then only then only `linear.predictor[idx]' is set
		 */
		for (i = 0; i < mb->predictor_n + mb->predictor_m; i++) {
			compute[count] = (char) 0;
			count++;
		}

		for (i = 0; i < mb->expert_n_cpo_idx; i++) {
			compute[mb->expert_cpo_idx[i]] = (char) 1;
			mb->d[mb->expert_cpo_idx[i]] = 0.0;
		}
		mb->ai_par->cpo_manual = 1;
		mb->output->hyperparameters = GMRFLib_FALSE;

		for (i = 0; i < mb->nf; i++) {
			if (mb->f_bfunc2[i]) {
				for (j = 0; j < mb->f_Ntotal[i]; j++) {
					bfunc[count + j] = Calloc(1, GMRFLib_bfunc_tp);
					bfunc[count + j]->bdef = mb->f_bfunc2[i];
					bfunc[count + j]->idx = j;
				}
			}
			for (j = 0; j < mb->f_Ntotal[i]; j++) {
				compute[count] = (char) 0;
				count++;
			}
		}

		for (i = 0; i < mb->nlinear; i++) {
			compute[count] = (char) 0;
			b[count] = mb->linear_precision[i] * mb->linear_mean[i];
			count++;
		}
		if (!mb->lc_derived_only) {
			for (i = 0; i < mb->nlc; i++) {
				compute[count++] = 0;
			}
		}
		assert(count == N);
	} else {
		/*
		 * as before 
		 */
		for (i = 0; i < mb->predictor_n + mb->predictor_m; i++) {
			compute[count++] = (char) mb->predictor_compute;
		}
		for (i = 0; i < mb->nf; i++) {
			if (mb->f_bfunc2[i]) {
				for (j = 0; j < mb->f_Ntotal[i]; j++) {
					bfunc[count + j] = Calloc(1, GMRFLib_bfunc_tp);
					bfunc[count + j]->bdef = mb->f_bfunc2[i];
					bfunc[count + j]->idx = j;
				}
			}
			for (j = 0; j < mb->f_Ntotal[i]; j++) {
				compute[count] = (char) mb->f_compute[i];
				count++;
			}
		}
		for (i = 0; i < mb->nlinear; i++) {
			compute[count] = (char) mb->linear_compute[i];
			b[count] = mb->linear_precision[i] * mb->linear_mean[i];
			count++;
		}
		if (!mb->lc_derived_only) {
			for (i = 0; i < mb->nlc; i++) {
				compute[count++] = 1;
			}
		}
		if (count != N) {
			P(count);
			P(N);
			assert(count == N);
		}
	}

	// correct using fixed effects only
	char *correct = NULL;
	if (mb->ai_par->correct) {
		int local_count = 0;
		correct = Calloc(N, char);
		count = mb->predictor_n + mb->predictor_m;
		for (i = 0; i < mb->nf; i++) {
			if ((mb->f_correct[i] < 0 && mb->f_Ntotal[i] == 1) || mb->f_correct[i] > 0) {
				/*
				 * add also random effects with size 1
				 */
				for (j = 0; j < mb->f_Ntotal[i]; j++) {
					correct[count + j] = (char) 1;
					local_count++;
				}
			}
			count += mb->f_Ntotal[i];
		}
		for (i = 0; i < mb->nlinear; i++) {
			correct[count++] = (char) 1;
			local_count++;
		}
		if (local_count == 0) {			       /* then there is nothting to correct for */
			Free(correct);
			correct = NULL;
		}
	}
	Free(mb->ai_par->correct);
	mb->ai_par->correct = correct;

	if (G.reorder < 0) {
		GMRFLib_sizeof_tp nnz = 0;
		int use_g = 0;
		GMRFLib_optimize_reorder(mb->hgmrfm->graph, &nnz, &use_g, &(mb->gn));
		if (mb->verbose) {
			printf("\tFound optimal reordering=[%s] nnz(L)=[%lu] and use_global_nodes(user)=[%s]\n", GMRFLib_reorder_name(GMRFLib_reorder), nnz,
			       (use_g ? "yes" : "no"));
		}
	}
	if (mb->verbose) {
		if (mb->ntheta) {
			printf("\tList of hyperparameters: \n");
			for (i = 0; i < mb->ntheta; i++) {
				printf("\t\ttheta[%1d] = [%s]\n", i, mb->theta_tag[i]);
			}
		} else {
			printf("\tNone hyperparameters\n");
		}
	}
	GMRFLib_ai_store_tp *ai_store = Calloc(1, GMRFLib_ai_store_tp);

	if (mb->output->dic) {
		mb->dic = Calloc(1, GMRFLib_ai_dic_tp);
	} else {
		mb->dic = NULL;
	}
	/*
	 * compute a 'reasonable' initial value for \eta, unless its there from before.
	 */
	x = Calloc(N, double);

	if (mb->reuse_mode && mb->x_file) {
		if (N != mb->nx_file) {
			char *msg;
			GMRFLib_sprintf(&msg, "N = %1d but nx_file = %1d. Stop.", N, mb->nx_file);
			inla_error_general(msg);
		}
		memcpy(x, mb->x_file, N * sizeof(double));
		/*
		 * subtract the offset 
		 */
		for (i = 0; i < mb->predictor_ndata; i++) {
			x[i] -= OFFSET3(i);
		}

	} else {
#pragma omp parallel for private(i)
		for (i = 0; i < mb->predictor_ndata; i++) {
			if (mb->d[i]) {
				x[i] = inla_compute_initial_value(i, mb->loglikelihood[i], x, (void *) mb->loglikelihood_arg[i]);
			} else {
				x[i] = 0.0;
			}
			// printf("initial value x[%1d] = %g\n", i, x[i]);
		}
	}

	/*
	 * set the flag to compute correlation-matrix or not 
	 */
	mb->misc_output = Calloc(1, GMRFLib_ai_misc_output_tp);
	if (mb->lc_derived_correlation_matrix) {
		mb->misc_output->compute_corr_lin = mb->nlc;   /* yes, pass the dimension */
	} else {
		mb->misc_output->compute_corr_lin = 0;
	}
	if (mb->output->config) {
		mb->misc_output->configs = Calloc(GMRFLib_MAX_THREADS, GMRFLib_store_configs_tp *);
	} else {
		mb->misc_output->configs = NULL;
	}

	if (mb->fixed_mode) {
		/*
		 * then there is a request to treat the theta's as fixed and known. This little hack do the job nicely. 
		 */
		mb->ntheta = 0;
		mb->data_ntheta_all = 0;
		mb->theta = NULL;
	}

	mb->transform_funcs = Calloc(N, GMRFLib_transform_array_func_tp *);
	for (i = 0; i < mb->predictor_m + mb->predictor_n; i++) {
		/*
		 * only where we have data (ie n or m), can be a invlinkfunc different from the identity. also the compute-flag must be ON.
		 */
		if (!compute[i]) {
			mb->transform_funcs[i] = NULL;
		} else if (i < mb->predictor_ndata && mb->predictor_invlinkfunc[i]) {
			mb->transform_funcs[i] = Calloc(1, GMRFLib_transform_array_func_tp);
			mb->transform_funcs[i]->func = (GMRFLib_transform_func_tp *) mb->predictor_invlinkfunc[i];
			mb->transform_funcs[i]->arg = mb->predictor_invlinkfunc_arg[i];

			double *cov = NULL;
			if (mb->predictor_invlinkfunc_covariates && mb->predictor_invlinkfunc_covariates[i]) {
				int ncov = mb->predictor_invlinkfunc_covariates[i]->ncol;
				cov = Calloc(ncov, double);
				GMRFLib_matrix_get_row(cov, i, mb->predictor_invlinkfunc_covariates[i]);
			}
			mb->transform_funcs[i]->cov = cov;     /* yes, we store a copy here */
		} else {
			mb->transform_funcs[i] = Calloc(1, GMRFLib_transform_array_func_tp);
			mb->transform_funcs[i]->func = (GMRFLib_transform_func_tp *) link_identity;
			mb->transform_funcs[i]->arg = NULL;
			mb->transform_funcs[i]->cov = NULL;
		}
	}

	/*
	 * Finally, let us do the job...
	 */
	GMRFLib_ai_INLA(&(mb->density), (mb->output->gdensity ? &(mb->gdensity) : NULL),
			// DISABLE THIS FEATURE NOW, IT DOES NOT WORK WELL ENOGUH
			(0 ? &(mb->density_transform) : NULL), (0 ? mb->transform_funcs : NULL),
			// ....
			(mb->output->hyperparameters ? &(mb->density_hyper) : NULL),
			(mb->output->cpo || mb->expert_cpo_manual ? &(mb->cpo) : NULL),
			(mb->output->po ? &(mb->po) : NULL),
			mb->dic,
			(mb->output->mlik ? &(mb->mlik) : NULL),
			&(mb->neffp),
			compute, mb->theta, mb->ntheta,
			extra, (void *) mb,
			x, b, c, NULL, bfunc, mb->d,
			loglikelihood_inla, (void *) mb, NULL,
			mb->hgmrfm->graph, mb->hgmrfm->Qfunc, mb->hgmrfm->Qfunc_arg, mb->hgmrfm->constr, mb->ai_par, ai_store,
			mb->nlc, mb->lc_lc, &(mb->density_lin), mb->misc_output);

	/*
	 * add the offsets to the linear predictor. Add the offsets to the 'configs' (if any), at a later stage. 
	 */
#pragma omp parallel for private(i)
	for (i = 0; i < mb->predictor_n + mb->predictor_m; i++) {
		GMRFLib_density_tp *d;

		if (mb->density[i]) {
			d = mb->density[i];
			GMRFLib_density_new_mean(&(mb->density[i]), d, d->std_mean + OFFSET3(i));
			GMRFLib_free_density(d);
		}
		if (mb->gdensity && mb->gdensity[i]) {
			d = mb->gdensity[i];
			GMRFLib_density_new_mean(&(mb->gdensity[i]), d, d->std_mean + OFFSET3(i));
			GMRFLib_free_density(d);
		}
	}

	/*
	 * add the offset to 'x' 
	 */
	for (i = 0; i < mb->predictor_n + mb->predictor_m; i++) {
		x[i] += OFFSET3(i);
	}

	Free(mb->x_file);				       /* yes, and then */
	mb->x_file = x;					       /* just take over */
	mb->nx_file = N;

	GMRFLib_free_ai_store(ai_store);
	Free(b);
	Free(c);
	Free(compute);

	return INLA_OK;
}
int inla_MCMC(inla_tp * mb_old, inla_tp * mb_new)
{
	double *c = NULL, *x_old = NULL, *x_new = NULL, *b = NULL;
	int N, i, ii, j, n, count;
	char *fnm, *msg;
	ssize_t rw_retval;

	if (mb_old->fixed_mode) {
		/*
		 * then there is a request to treat the theta's as fixed and known. This little hack do the job nicely. 
		 */
		mb_old->ntheta = 0;
		mb_old->theta = NULL;

		assert(mb_new->fixed_mode);
		mb_new->ntheta = 0;
		mb_new->theta = NULL;
	}

	for (i = 0; i < mb_old->nf; i++) {
		if (mb_old->f_bfunc2[i]) {
			fprintf(stderr, "\n\n\t*** The MCMC-module is not yet implemented with mfunc-models. Please contact developers\n\n");
			exit(EXIT_FAILURE);
		}
	}

	if (mb_old->verbose) {
		printf("Enter %s... with scale=[%.5f] thinning=[%1d] niter=[%1d] num.threads=[%1d]\n",
		       __GMRFLib_FuncName, G.mcmc_scale, G.mcmc_thinning, G.mcmc_niter, GMRFLib_MAX_THREADS);
	}
	GMRFLib_init_hgmrfm(&(mb_old->hgmrfm), mb_old->predictor_n, mb_old->predictor_m,
			    mb_old->predictor_cross_sumzero, NULL, mb_old->predictor_log_prec,
			    (const char *) mb_old->predictor_Aext_fnm, mb_old->predictor_Aext_precision,
			    mb_old->nf, mb_old->f_c, mb_old->f_weights, mb_old->f_graph, mb_old->f_Qfunc, mb_old->f_Qfunc_arg, mb_old->f_sumzero, mb_old->f_constr,
			    mb_old->ff_Qfunc, mb_old->ff_Qfunc_arg,
			    mb_old->nlinear, mb_old->linear_covariate, mb_old->linear_precision,
			    (mb_old->lc_derived_only ? 0 : mb_old->nlc), mb_old->lc_lc, mb_old->lc_prec, mb_old->ai_par);
	GMRFLib_init_hgmrfm(&(mb_new->hgmrfm), mb_new->predictor_n, mb_new->predictor_m,
			    mb_new->predictor_cross_sumzero, NULL, mb_new->predictor_log_prec,
			    (const char *) mb_new->predictor_Aext_fnm, mb_new->predictor_Aext_precision,
			    mb_new->nf, mb_new->f_c, mb_new->f_weights, mb_new->f_graph, mb_new->f_Qfunc, mb_new->f_Qfunc_arg, mb_new->f_sumzero, mb_new->f_constr,
			    mb_new->ff_Qfunc, mb_new->ff_Qfunc_arg,
			    mb_new->nlinear, mb_new->linear_covariate, mb_new->linear_precision,
			    (mb_new->lc_derived_only ? 0 : mb_new->nlc), mb_new->lc_lc, mb_new->lc_prec, mb_old->ai_par);

	N = ((GMRFLib_hgmrfm_arg_tp *) mb_new->hgmrfm->Qfunc_arg)->N;
	assert(N == ((GMRFLib_hgmrfm_arg_tp *) mb_old->hgmrfm->Qfunc_arg)->N);	/* just a check */

	if (mb_new->verbose) {
		printf("\tSize of full graph=[%1d]\n", N);
	}

	mb_old->d = Realloc(mb_old->d, N, double);
	memset(&(mb_old->d[mb_old->predictor_ndata]), 0, (N - mb_old->predictor_ndata) * sizeof(double));
	mb_old->loglikelihood = Realloc(mb_old->loglikelihood, N, GMRFLib_logl_tp *);
	memset(&(mb_old->loglikelihood[mb_old->predictor_ndata]), 0, (N - mb_old->predictor_ndata) * sizeof(GMRFLib_logl_tp *));
	mb_old->loglikelihood_arg = Realloc(mb_old->loglikelihood_arg, N, void *);
	memset(&(mb_old->loglikelihood_arg[mb_old->predictor_ndata]), 0, (N - mb_old->predictor_ndata) * sizeof(void *));
	mb_new->d = Realloc(mb_new->d, N, double);
	memset(&(mb_new->d[mb_new->predictor_ndata]), 0, (N - mb_new->predictor_ndata) * sizeof(double));
	mb_new->loglikelihood = Realloc(mb_new->loglikelihood, N, GMRFLib_logl_tp *);
	memset(&(mb_new->loglikelihood[mb_old->predictor_ndata]), 0, (N - mb_old->predictor_ndata) * sizeof(GMRFLib_logl_tp *));
	mb_new->loglikelihood_arg = Realloc(mb_new->loglikelihood_arg, N, void *);
	memset(&(mb_new->loglikelihood_arg[mb_new->predictor_ndata]), 0, (N - mb_new->predictor_ndata) * sizeof(void *));

	/*
	 * add the diagonal, if any 
	 */
	c = Calloc(N, double);
	count = mb_new->predictor_n + mb_new->predictor_m;
	for (i = 0; i < mb_new->nf; i++) {
		for (j = 0; j < mb_new->f_n[i]; j++) {
			c[count + j] = mb_new->f_diag[i];      /* yes; this is correct */
		}
		count += mb_new->f_Ntotal[i];		       /* yes; this is correct */
	}

	/*
	 * this is an emergency option to prevent singular matrices (and is known to be >= 0). 
	 */
	if (mb_new->expert_diagonal_emergencey) {
		for (i = mb_new->predictor_n + mb_new->predictor_m; i < N; i++)
			c[i] += mb_new->expert_diagonal_emergencey;
	}

	/*
	 * compute the b
	 */
	b = Calloc(N, double);

	count = 0;
	for (i = 0; i < mb_new->predictor_n + mb_new->predictor_m; i++) {
		count++;
	}
	for (i = 0; i < mb_new->nf; i++) {
		for (j = 0; j < mb_new->f_Ntotal[i]; j++) {
			count++;
		}
	}
	for (i = 0; i < mb_new->nlinear; i++) {
		b[count] = mb_new->linear_precision[i] * mb_new->linear_mean[i];
		count++;
	}
	for (i = 0; i < mb_new->nlc; i++) {
		count++;
	}
	assert(count == N);

	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(mb_new->hgmrfm->graph, NULL, NULL, &(mb_new->gn));
		if (mb_new->verbose) {
			printf("\tFound optimal reordering=[%s]\n", GMRFLib_reorder_name(GMRFLib_reorder));
		}
	}
	if (mb_new->verbose) {
		if (mb_new->ntheta) {
			printf("\tList of hyperparameters: \n");
			for (i = 0; i < mb_new->ntheta; i++) {
				printf("\t\ttheta[%1d] = [%s]\n", i, mb_new->theta_tag[i]);
			}
		} else {
			printf("\tNone hyperparameters\n");
		}
	}

	x_old = Calloc(N, double);
	x_new = Calloc(N, double);

	if (mb_old->reuse_mode && mb_old->x_file) {
		if (N != mb_old->nx_file) {
			GMRFLib_sprintf(&msg, "N = %1d but nx_file = %1d. Stop.", N, mb_old->nx_file);
			inla_error_general(msg);
		}
		memcpy(x_old, mb_old->x_file, N * sizeof(double));
		memcpy(x_new, mb_old->x_file, N * sizeof(double));

		/*
		 * subtract the offset 
		 */
		for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++) {
			x_old[i] -= OFFSET2(i);
			x_new[i] -= OFFSET2(i);
		}
	} else {
#pragma omp parallel for private(i)
		for (i = 0; i < mb_old->predictor_ndata; i++) {
			if (mb_old->d[i]) {
				x_old[i] = x_new[i] = inla_compute_initial_value(i, mb_old->loglikelihood[i], NULL, (void *) mb_old->loglikelihood_arg[i]);
			} else {
				x_old[i] = x_new[i] = 0.0;
			}
		}
	}

	GMRFLib_ai_store_tp *ai_store = Calloc(1, GMRFLib_ai_store_tp);
	mb_old->ai_par->int_strategy = GMRFLib_AI_INT_STRATEGY_EMPIRICAL_BAYES;
	mb_old->ai_par->strategy = GMRFLib_AI_STRATEGY_GAUSSIAN;

	GMRFLib_ai_INLA(&(mb_old->density), &(mb_old->gdensity), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, mb_old->theta, mb_old->ntheta,
			extra, (void *) mb_old, x_old, b, c, NULL, NULL, mb_old->d, loglikelihood_inla, (void *) mb_old, NULL,
			mb_old->hgmrfm->graph, mb_old->hgmrfm->Qfunc, mb_old->hgmrfm->Qfunc_arg, mb_old->hgmrfm->constr, mb_old->ai_par, ai_store,
			0, NULL, NULL, NULL);
	GMRFLib_free_ai_store(ai_store);

	/*
	 * set better initial values 
	 */
	if (mb_old->gdensity) {
		for (i = 0; i < N; i++) {
			if (mb_old->gdensity[i]) {
				x_old[i] = mb_old->gdensity[i]->user_mean;
			}
		}
	}

	if (mb_old->ntheta) {
		printf("\n\tInitial values for the hyperparameters: \n");
		for (i = 0; i < mb_old->ntheta; i++) {
			printf("\t\ttheta[%1d] = %s = %.10g (= %.10g in user-scale)\n", i, mb_old->theta_tag[i], mb_old->theta[i][0][0],
			       mb_old->theta_map[i] (mb_old->theta[i][0][0], MAP_FORWARD, mb_old->theta_map_arg[i]));
		}
		printf("\n");
	}

	/*
	 * compute the offset for each pack of the results 
	 */
	int len_offsets = 2 + mb_old->nf + mb_old->nlinear + mb_old->nlc;
	int *offsets = Calloc(len_offsets, int);

	j = n = 0;
	offsets[0] = 0;
	n += mb_old->predictor_n + mb_old->predictor_m;
	for (i = 0; i < mb_old->nf; i++) {
		offsets[++j] = n;
		n += mb_old->f_graph[i]->n;
	}
	for (i = 0; i < mb_old->nlinear; i++) {
		offsets[++j] = n;
		n++;
	}
	for (i = 0; i < mb_old->nlc; i++) {
		offsets[++j] = n;
		n++;
	}
	offsets[++j] = n;

	assert(mb_old->hgmrfm->graph->n == n);
	assert(j == len_offsets - 1);

	if (mb_old->verbose) {
		printf("\tstore results in directory[%s]\n", mb_old->dir);
	}

	FILE **fpp = Calloc(len_offsets + 2 * mb_old->ntheta, FILE *);

	/*
	 * Store the offsets in the linear predictor to file 
	 */
	FILE *fp_offset;
	GMRFLib_sprintf(&fnm, "%s/totaloffset", mb_old->dir);
	inla_fnmfix(fnm);
	inla_mkdir(fnm);
	Free(fnm);
	GMRFLib_sprintf(&fnm, "%s/totaloffset/totaloffset.dat", mb_old->dir);
	inla_fnmfix(fnm);
	fp_offset = fopen(fnm, "w");
	Free(fnm);
	for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++) {
		fprintf(fp_offset, "%d %.12g\n", i, OFFSET2(i));
	}
	fclose(fp_offset);

	j = -1;						       /* yes */
	j++;
	if (mb_old->predictor_compute) {
		GMRFLib_sprintf(&fnm, "%s/%s", mb_old->dir, mb_old->predictor_dir);
		inla_fnmfix(fnm);
		inla_mkdir(fnm);
		Free(fnm);
		GMRFLib_sprintf(&fnm, "%s/%s/trace.dat", mb_old->dir, mb_old->predictor_dir);
		inla_fnmfix(fnm);
		fpp[j] = fopen(fnm, "w");
		Free(fnm);
	}
	for (i = 0; i < mb_old->nf; i++) {
		j++;
		if (mb_old->f_compute[i]) {
			GMRFLib_sprintf(&fnm, "%s/%s", mb_old->dir, mb_old->f_dir[i]);
			inla_fnmfix(fnm);
			inla_mkdir(fnm);
			Free(fnm);
			GMRFLib_sprintf(&fnm, "%s/%s/trace.dat", mb_old->dir, mb_old->f_dir[i]);
			inla_fnmfix(fnm);
			fpp[j] = fopen(fnm, "w");
			Free(fnm);
		}
	}
	for (i = 0; i < mb_old->nlinear; i++) {
		j++;
		if (mb_old->linear_compute[i]) {
			GMRFLib_sprintf(&fnm, "%s/%s", mb_old->dir, mb_old->linear_dir[i]);
			inla_fnmfix(fnm);
			inla_mkdir(fnm);
			Free(fnm);
			GMRFLib_sprintf(&fnm, "%s/%s/trace.dat", mb_old->dir, mb_old->linear_dir[i]);
			inla_fnmfix(fnm);
			fpp[j] = fopen(fnm, "w");
			Free(fnm);
		}
	}
	for (i = 0; i < mb_old->nlc; i++) {
		j++;
		GMRFLib_sprintf(&fnm, "%s/%s", mb_old->dir, mb_old->lc_dir[i]);
		inla_fnmfix(fnm);
		inla_mkdir(fnm);
		Free(fnm);
		GMRFLib_sprintf(&fnm, "%s/%s/trace.dat", mb_old->dir, mb_old->lc_dir[i]);
		inla_fnmfix(fnm);
		fpp[j] = fopen(fnm, "w");
		Free(fnm);
	}
	for (i = 0; i < mb_old->ntheta; i++) {
		j++;
		GMRFLib_sprintf(&fnm, "%s/hyperparameter 1 %6.6d %s", mb_old->dir, i, mb_old->theta_dir[i]);
		inla_fnmfix(fnm);
		inla_mkdir(fnm);
		Free(fnm);
		GMRFLib_sprintf(&fnm, "%s/hyperparameter 1 %6.6d %s/trace.dat", mb_old->dir, i, mb_old->theta_dir[i]);
		inla_fnmfix(fnm);
		fpp[j] = fopen(fnm, "w");
		assert(fpp[j]);
		Free(fnm);

		j++;
		GMRFLib_sprintf(&fnm, "%s/hyperparameter 2 %6.6d %s user scale", mb_old->dir, i, mb_old->theta_dir[i]);
		inla_fnmfix(fnm);
		inla_mkdir(fnm);
		Free(fnm);
		GMRFLib_sprintf(&fnm, "%s/hyperparameter 2 %6.6d %s user scale/trace.dat", mb_old->dir, i, mb_old->theta_dir[i]);
		inla_fnmfix(fnm);
		fpp[j] = fopen(fnm, "w");
		assert(fpp[j]);
		Free(fnm);
	}

#define SET_THETA(_mb, _theta)						\
	if (1) {							\
		int _i, _j;						\
		for(_i=0; _i < _mb->ntheta; _i++) {			\
			for(_j=0; _j < GMRFLib_MAX_THREADS; _j++) {	\
				_mb->theta[_i][_j][0] = _theta[_i];	\
			}						\
		}							\
	}
#define GET_THETA(_mb, _theta)					\
	if (1) {						\
		int _i;						\
		for(_i=0; _i < _mb->ntheta; _i++) {		\
			_theta[_i] = _mb->theta[_i][0][0];	\
		}						\
	}

	GMRFLib_store_tp *store = NULL;
	store = Calloc(1, GMRFLib_store_tp);
	store->store_problems = GMRFLib_TRUE;

	double pav = 0.0, tref = GMRFLib_cpu();
	double *theta_new = NULL, *theta_old = NULL;
	int iteration = 0, update_theta;

	if (mb_old->ntheta) {
		theta_new = Calloc(mb_new->ntheta, double);
		theta_old = Calloc(mb_old->ntheta, double);
		GET_THETA(mb_old, theta_old);		       /* get theta_old */
		SET_THETA(mb_new, theta_old);
	}

	update_theta = mb_old->ntheta && (mb_new->ai_par->int_strategy != GMRFLib_AI_INT_STRATEGY_EMPIRICAL_BAYES);	/* yes, use mb_new */
	store->fixed_hyperparameters = !update_theta;

#if !defined(WINDOWS)
	int fifo_get = -1;
	int fifo_put = -1;
	int fifo_err = -1;
	double *all_fifo_get = NULL;
	double *all_fifo_put = NULL;

	int fifo_get_data = -1;
	int fifo_put_data = -1;
	double *all_data_fifo_get = NULL;
	double *all_data_fifo_put = NULL;

	if (G.mcmc_fifo) {
		remove(FIFO_GET);
		remove(FIFO_PUT);

		if (mb_old->verbose) {
			printf("Create new fifo-file [%s]\n", FIFO_GET);
		}
		fifo_err = mkfifo(FIFO_GET, 0700);
		if (fifo_err) {
			GMRFLib_sprintf(&msg, "FIFO-ERROR (GET): %s\n", strerror(errno));
			inla_error_general(msg);
			exit(EXIT_FAILURE);
		}
		if (mb_old->verbose) {
			printf("Create new fifo-file [%s]\n", FIFO_PUT);
		}
		fifo_err = mkfifo(FIFO_PUT, 0700);
		if (fifo_err) {
			GMRFLib_sprintf(&msg, "FIFO-ERROR (PUT): %s\n", strerror(errno));
			inla_error_general(msg);
			exit(EXIT_FAILURE);
		}

		if (mb_old->verbose) {
			printf("Open fifo file [%s]\n", FIFO_GET);
		}
		fifo_get = open(FIFO_GET, O_RDONLY);

		if (mb_old->verbose) {
			printf("Open fifo file [%s]\n", FIFO_PUT);
		}
		fifo_put = open(FIFO_PUT, O_WRONLY);

		fprintf(stderr, "\n\nNumber of doubles to pass through %s and %s: %1d\n\n", FIFO_GET, FIFO_PUT, mb_old->ntheta + N);
		all_fifo_get = Calloc(mb_old->ntheta + N, double);
		all_fifo_put = Calloc(mb_old->ntheta + N, double);

		if (G.mcmc_fifo_pass_data) {

			remove(FIFO_GET_DATA);
			remove(FIFO_PUT_DATA);

			if (mb_old->verbose) {
				printf("Create new fifo-file [%s]\n", FIFO_GET_DATA);
			}
			fifo_err = mkfifo(FIFO_GET_DATA, 0700);
			if (fifo_err) {
				GMRFLib_sprintf(&msg, "FIFO-ERROR (GET DATA): %s\n", strerror(errno));
				inla_error_general(msg);
				exit(EXIT_FAILURE);
			}
			if (mb_old->verbose) {
				printf("Create new fifo-file [%s]\n", FIFO_PUT_DATA);
			}
			fifo_err = mkfifo(FIFO_PUT_DATA, 0700);
			if (fifo_err) {
				GMRFLib_sprintf(&msg, "FIFO-ERROR (PUT DATA): %s\n", strerror(errno));
				inla_error_general(msg);
				exit(EXIT_FAILURE);
			}

			if (mb_old->verbose) {
				printf("Open fifo file [%s]\n", FIFO_GET_DATA);
			}
			fifo_get_data = open(FIFO_GET_DATA, O_RDONLY);

			if (mb_old->verbose) {
				printf("Open fifo file [%s]\n", FIFO_PUT_DATA);
			}
			fifo_put_data = open(FIFO_PUT_DATA, O_WRONLY);

			/*
			 * for each data_section, pass the data and the offset 
			 */
			fprintf(stderr, "\n\nNumber of doubles to pass through %s and %s: %1d\n\n",
				FIFO_GET_DATA, FIFO_PUT_DATA, 2 * mb_old->nds * mb_old->predictor_ndata);

			all_data_fifo_put = Calloc(2 * mb_old->nds * mb_old->predictor_ndata, double);
			all_data_fifo_get = Calloc(2 * mb_old->nds * mb_old->predictor_ndata, double);

			/*
			 * in this case, this feature cannot be used 
			 */
			store->store_problems = GMRFLib_FALSE;
		}
	}
#endif

	while (!G.mcmc_niter || iteration < G.mcmc_niter) {
		double lacc, p;

		iteration++;
		if (update_theta) {
			for (j = 0; j < mb_old->ntheta; j++) {
				theta_new[j] = theta_old[j] + G.mcmc_scale * (2 * (GMRFLib_uniform() - 0.5));
			}
			SET_THETA(mb_new, theta_new);
		}
		GMRFLib_blockupdate_store(&lacc,
					  x_new, x_old,
					  b, b,
					  c, c,
					  NULL, NULL,
					  mb_old->d, mb_old->d,
					  loglikelihood_inla, (void *) mb_new,
					  loglikelihood_inla, (void *) mb_old,
					  NULL,
					  mb_new->hgmrfm->graph,
					  mb_new->hgmrfm->Qfunc, mb_new->hgmrfm->Qfunc_arg,
					  mb_old->hgmrfm->Qfunc, mb_old->hgmrfm->Qfunc_arg,
					  NULL, NULL, NULL, NULL, mb_new->hgmrfm->constr, mb_old->hgmrfm->constr, NULL, NULL, store);
		if (update_theta) {
			lacc += extra(theta_new, mb_new->ntheta, (void *) mb_new) - extra(theta_old, mb_old->ntheta, (void *) mb_old);
		}
		p = exp(DMIN(0, lacc));
		if (iteration < 5 || GMRFLib_uniform() < p) {
			memcpy(x_old, x_new, N * sizeof(double));
			if (update_theta) {
				SET_THETA(mb_old, theta_new);
				GET_THETA(mb_old, theta_old);
			}
			store->decision = GMRFLib_STORE_ACCEPT;
		} else {
			store->decision = GMRFLib_STORE_REJECT;
		}

		pav = ((iteration - 1.0) * pav + p) / iteration;

		if ((G.mcmc_niter > 0 && iteration % IMAX(1, G.mcmc_niter / 20) == 0) || (G.mcmc_niter == 0 && iteration % IMAX(500, G.mcmc_thinning) == 0)) {
			printf("iteration %1d accept-rate: %f  iteration/s: %g \t||\t ", iteration, pav, iteration / (GMRFLib_cpu() - tref));
			for (j = 0; j < mb_old->ntheta; j++) {
				printf("%.4g ", theta_old[j]);
			}
			printf("\n");
		}

		if (iteration % G.mcmc_thinning == 0) {
			j = -1;				       /* yes */
			j++;
			if (fpp[j]) {
				for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++) {
					fprintf(fpp[j], " %.5f", x_old[i] + OFFSET2(i));
				}
				fprintf(fpp[j], "\n");
			}
			for (i = 0; i < mb_old->nf; i++) {
				j++;
				if (fpp[j]) {
					for (ii = offsets[j]; ii < offsets[j + 1]; ii++) {
						fprintf(fpp[j], " %.4g", x_old[ii]);
					}
					fprintf(fpp[j], "\n");
				}
			}
			for (i = 0; i < mb_old->nlinear; i++) {
				j++;
				if (fpp[j]) {
					ii = offsets[j];
					fprintf(fpp[j], " %.4g\n", x_old[ii]);
				}
			}
			for (i = 0; i < mb_old->nlc; i++) {
				j++;
				if (fpp[j]) {
					ii = offsets[j];
					fprintf(fpp[j], " %.4g\n", x_old[ii]);
				}
			}
			for (i = 0; i < mb_old->ntheta; i++) {
				j++;
				if (update_theta && fpp[j]) {
					fprintf(fpp[j], "%.5f\n", mb_old->theta[i][0][0]);
				}

				j++;
				if (update_theta && fpp[j]) {
					fprintf(fpp[j], "%.5f\n", mb_old->theta_map[i] (mb_old->theta[i][0][0], MAP_FORWARD, mb_old->theta_map_arg[i]));
				}
			}
			assert(j == len_offsets - 2 + 2 * mb_old->ntheta);

			if (iteration % 1000 == 0) {
				for (j = 0; j < len_offsets + 2 * mb_old->ntheta; j++) {
					if (fpp[j]) {
						fflush(fpp[j]);
					}
				}
			}
		}
#if !defined(WINDOWS)
		if (G.mcmc_fifo) {
			/*
			 * send all variables in all_ = c( theta, x) to an external process for processing, and replace it with what is coming back. 
			 */

			for (i = 0; i < mb_old->ntheta; i++) {
				all_fifo_put[i] = mb_old->theta[i][0][0];
			}
			for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++) {
				all_fifo_put[i + mb_old->ntheta] = x_old[i] + OFFSET2(i);	/* yes, add the offset */
			}
			for (i = mb_old->predictor_n + mb_old->predictor_m; i < N; i++) {
				all_fifo_put[i + mb_old->ntheta] = x_old[i];
			}

			/*
			 * can use the same vector here, but this enable us to see what is changed. 
			 */
			rw_retval = write(fifo_put, all_fifo_put, (N + mb_old->ntheta) * sizeof(double));
			assert(rw_retval);
			rw_retval = read(fifo_get, all_fifo_get, (N + mb_old->ntheta) * sizeof(double));
			assert(rw_retval);

			if (mb_old->verbose) {
				if (0) {
					for (i = 0; i < N + mb_old->ntheta; i++)
						printf("fifo: put xxx[%1d] = %.12g  get %.12g\n", i, all_fifo_get[i], all_fifo_put[i]);
				}
			}

			for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++)
				x_old[i] = all_fifo_get[i + mb_old->ntheta] - OFFSET2(i);
			for (i = mb_old->predictor_n + mb_old->predictor_m; i < N; i++) {
				x_old[i] = all_fifo_get[i + mb_old->ntheta];
			}
			SET_THETA(mb_old, all_fifo_get);

			if (G.mcmc_fifo_pass_data) {

				int dlen = 2 * mb_old->nds * mb_old->predictor_ndata;

				for (i = 0; i < mb_old->nds; i++) {
					memcpy(&(all_data_fifo_put[2 * i * mb_old->predictor_ndata]),
					       mb_old->data_sections[i].data_observations.y, mb_old->predictor_ndata * sizeof(double));
					memcpy(&(all_data_fifo_put[2 * i * mb_old->predictor_ndata + mb_old->predictor_ndata]),
					       mb_old->data_sections[i].offset, mb_old->predictor_ndata * sizeof(double));
				}

				rw_retval = write(fifo_put_data, all_data_fifo_put, dlen * sizeof(double));
				assert(rw_retval);
				rw_retval = read(fifo_get_data, all_data_fifo_get, dlen * sizeof(double));
				assert(rw_retval);

				if (mb_old->verbose) {
					if (0) {
						for (i = 0; i < dlen; i++)
							printf("fifo: put data[%1d] = %.12g  get %.12g\n", i, all_data_fifo_get[i], all_data_fifo_put[i]);
					}
				}
				for (i = 0; i < mb_old->nds; i++) {
					memcpy(mb_old->data_sections[i].data_observations.y,
					       &(all_data_fifo_get[2 * i * mb_old->predictor_ndata]), mb_old->predictor_ndata * sizeof(double));
					memcpy(mb_old->data_sections[i].offset,
					       &(all_data_fifo_get[2 * i * mb_old->predictor_ndata + mb_old->predictor_ndata]),
					       mb_old->predictor_ndata * sizeof(double));
				}
			}
		}
#endif
	}

	/*
	 * write to file, the last configuration additional to the index-table, describing what is what. 
	 */
	FILE *fp_last_x = NULL;
	FILE *fp_last_theta = NULL;
	FILE *fp_idx_table = NULL;
	char *last_dir = NULL;
	char *last_theta = NULL;
	char *last_x = NULL;
	char *idx_table = NULL;

	GMRFLib_sprintf(&last_dir, "%s/%s", mb_old->dir, "last-mcmc-configuration");
	inla_fnmfix(last_dir);
	inla_mkdir(last_dir);

	GMRFLib_sprintf(&last_theta, "%s/theta.dat", last_dir);
	inla_fnmfix(last_theta);
	fp_last_theta = fopen(last_theta, "w");
	for (i = 0; i < mb_old->ntheta; i++) {
		fprintf(fp_last_theta, "%.12g\n", mb_old->theta[i][0][0]);
	}
	fclose(fp_last_theta);

	GMRFLib_sprintf(&last_x, "%s/x.dat", last_dir);
	inla_fnmfix(last_x);
	fp_last_x = fopen(last_x, "w");
	for (i = 0; i < mb_old->predictor_n + mb_old->predictor_m; i++) {
		fprintf(fp_last_x, "%.12g\n", x_old[i] + OFFSET2(i));
	}
	for (i = mb_old->predictor_n + mb_old->predictor_m; i < N; i++) {
		fprintf(fp_last_x, "%.12g\n", x_old[i]);
	}
	fclose(fp_last_x);

	GMRFLib_sprintf(&idx_table, "%s/idx-table.dat", last_dir);
	inla_fnmfix(idx_table);
	fp_idx_table = fopen(idx_table, "w");

	for (i = 0; i < mb_old->idx_tot; i++) {
		fprintf(fp_idx_table, "%1d %1d %s\n", mb_old->idx_start[i], mb_old->idx_n[i], mb_old->idx_tag[i]);
	}
	fclose(fp_idx_table);

	/*
	 * cleanup 
	 */
	GMRFLib_free_store(store);
	Free(b);
	Free(c);
	Free(x_old);
	Free(x_new);
	Free(theta_old);
	Free(theta_new);
	Free(last_dir);
	Free(last_theta);
	Free(last_x);
	Free(idx_table);

	return INLA_OK;
}
int inla_parse_output(inla_tp * mb, dictionary * ini, int sec, Output_tp ** out)
{
	/*
	 * parse the output-options. defaults are given in the type=output-section, which are initialised with program defaults if
	 * the program defaults are NULL. 
	 */
	int i, j, use_defaults = 1;
	char *secname = NULL, *tmp = NULL;

	secname = GMRFLib_strdup(iniparser_getsecname(ini, sec));
	if (!mb->output) {
		/*
		 * set default options 
		 */
		assert(mb->output == *out);
		use_defaults = 1;			       /* to flag that we're reading mb->output */
		(*out) = Calloc(1, Output_tp);
		(*out)->cpo = 0;
		(*out)->po = 0;
		(*out)->dic = 0;
		(*out)->summary = 1;
		(*out)->return_marginals = 1;
		(*out)->kld = 1;
		(*out)->mlik = 0;
		(*out)->q = 0;
		(*out)->graph = 0;
		(*out)->config = 0;
		(*out)->gdensity = 0;
		(*out)->hyperparameters = (G.mode == INLA_MODE_HYPER ? 1 : 1);
		(*out)->mode = 1;
		(*out)->nquantiles = 0;
		(*out)->ncdf = 0;
		(*out)->quantiles = (*out)->cdf = NULL;
	} else {
		use_defaults = 0;
		*out = Calloc(1, Output_tp);
		(*out)->cpo = mb->output->cpo;
		(*out)->po = mb->output->po;
		(*out)->dic = mb->output->dic;
		(*out)->summary = mb->output->summary;
		(*out)->kld = mb->output->kld;
		(*out)->mlik = mb->output->mlik;
		(*out)->q = mb->output->q;
		(*out)->graph = mb->output->graph;
		(*out)->config = mb->output->config;
		(*out)->gdensity = mb->output->gdensity;
		(*out)->hyperparameters = mb->output->hyperparameters;
		(*out)->mode = mb->output->mode;
		(*out)->return_marginals = mb->output->return_marginals;
		(*out)->nquantiles = mb->output->nquantiles;
		if (mb->output->nquantiles) {
			(*out)->quantiles = Calloc(mb->output->nquantiles, double);
			memcpy((*out)->quantiles, mb->output->quantiles, (size_t) mb->output->nquantiles * sizeof(double));
		}
		(*out)->ncdf = mb->output->ncdf;
		if (mb->output->ncdf) {
			(*out)->cdf = Calloc(mb->output->ncdf, double);
			memcpy((*out)->cdf, mb->output->cdf, (size_t) mb->output->ncdf * sizeof(double));
		}
	}
	(*out)->cpo = iniparser_getboolean(ini, inla_string_join(secname, "CPO"), (*out)->cpo);
	(*out)->po = iniparser_getboolean(ini, inla_string_join(secname, "PO"), (*out)->po);
	(*out)->dic = iniparser_getboolean(ini, inla_string_join(secname, "DIC"), (*out)->dic);
	(*out)->summary = iniparser_getboolean(ini, inla_string_join(secname, "SUMMARY"), (*out)->summary);
	(*out)->return_marginals = iniparser_getboolean(ini, inla_string_join(secname, "RETURN.MARGINALS"), (*out)->return_marginals);
	(*out)->hyperparameters = iniparser_getboolean(ini, inla_string_join(secname, "HYPERPARAMETERS"), (*out)->hyperparameters);
	(*out)->mode = iniparser_getboolean(ini, inla_string_join(secname, "MODE"), (*out)->mode);
	(*out)->kld = iniparser_getboolean(ini, inla_string_join(secname, "KLD"), (*out)->kld);
	(*out)->mlik = iniparser_getboolean(ini, inla_string_join(secname, "MLIK"), (*out)->mlik);
	(*out)->q = iniparser_getboolean(ini, inla_string_join(secname, "Q"), (*out)->q);
	(*out)->graph = iniparser_getboolean(ini, inla_string_join(secname, "GRAPH"), (*out)->graph);
	(*out)->config = iniparser_getboolean(ini, inla_string_join(secname, "CONFIG"), (*out)->config);
	(*out)->gdensity = iniparser_getboolean(ini, inla_string_join(secname, "GDENSITY"), (*out)->gdensity);
	tmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "QUANTILES"), NULL));

	if (G.mode == INLA_MODE_HYPER) {
		/*
		 * these are the requirements for the HYPER_MODE 
		 */
		(*out)->cpo = 0;
		(*out)->po = 0;
		(*out)->dic = 0;
		(*out)->mlik = 1;
	}
	if (G.mode == INLA_MODE_HYPER) {
		if (!((*out)->hyperparameters)) {
			fprintf(stderr, "*** Warning: HYPER_MODE require (*out)->hyperparameters = 1\n");
		}
		(*out)->hyperparameters = 1;
	}

	if (tmp) {
		inla_sread_doubles_q(&((*out)->quantiles), &((*out)->nquantiles), tmp);

		if ((*out)->nquantiles == 0)
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "quantiles", tmp);

		for (j = 0; j < (*out)->nquantiles; j++) {
			if ((*out)->quantiles[j] <= 0.0 || (*out)->quantiles[j] >= 1.0) {
				inla_error_field_is_void(__GMRFLib_FuncName, secname, "quantiles", tmp);
			}
		}
	}
	tmp = GMRFLib_strdup(iniparser_getstring(ini, inla_string_join(secname, "CDF"), NULL));
	if (tmp) {
		inla_sread_doubles_q(&((*out)->cdf), &((*out)->ncdf), tmp);
		if ((*out)->ncdf == 0) {
			inla_error_field_is_void(__GMRFLib_FuncName, secname, "cdf", tmp);
		}
	}
	if (mb->verbose) {
		printf("\t\toutput:\n");
		if (use_defaults) {
			printf("\t\t\tcpo=[%1d]\n", (*out)->cpo);
			printf("\t\t\tpo=[%1d]\n", (*out)->po);
			printf("\t\t\tdic=[%1d]\n", (*out)->dic);
			printf("\t\t\tkld=[%1d]\n", (*out)->kld);
			printf("\t\t\tmlik=[%1d]\n", (*out)->mlik);
			printf("\t\t\tq=[%1d]\n", (*out)->q);
			printf("\t\t\tgraph=[%1d]\n", (*out)->graph);
			printf("\t\t\tgdensity=[%1d]\n", (*out)->gdensity);
			printf("\t\t\thyperparameters=[%1d]\n", (*out)->hyperparameters);
		}
		printf("\t\t\tsummary=[%1d]\n", (*out)->summary);
		printf("\t\t\treturn.marginals=[%1d]\n", (*out)->return_marginals);
		printf("\t\t\tnquantiles=[%1d]  [", (*out)->nquantiles);
		for (i = 0; i < (*out)->nquantiles; i++) {
			printf(" %g", (*out)->quantiles[i]);
		}
		printf(" ]\n");
		printf("\t\t\tncdf=[%1d]  [", (*out)->ncdf);
		for (i = 0; i < (*out)->ncdf; i++) {
			printf(" %g", (*out)->cdf[i]);
		}
		printf(" ]\n");
	}
	return INLA_OK;
}
int inla_computed(GMRFLib_density_tp ** d, int n)
{
	/*
	 * return 0 if all d[i]'s are NULL, and 1 otherwise. 
	 */
	int i;

	if (!d) {
		return INLA_OK;
	}
	for (i = 0; i < n; i++) {
		if (d[i]) {
			return INLA_FAIL;
		}
	}
	return INLA_OK;
}
int inla_output_Q(inla_tp * mb, const char *dir, GMRFLib_graph_tp * graph)
{
	GMRFLib_problem_tp *p = NULL;
	char *fnm = NULL, *newdir = NULL;
	FILE *fp = NULL;

	GMRFLib_init_problem(&p, NULL, NULL, NULL, NULL, graph, GMRFLib_Qfunc_generic, (void *) graph, NULL, NULL, GMRFLib_NEW_PROBLEM);
	GMRFLib_sprintf(&newdir, "%s/Q", dir);
	if (mb->verbose) {
		printf("\t\tstore factorisation results in[%s]\n", newdir);
	}
	if (inla_mkdir(newdir) == INLA_OK) {
		if (mb->output->q) {
			if (mb->verbose) {
				printf("\t\tstore info precision and related matrices in[%s]\n", newdir);
			}
			GMRFLib_sprintf(&fnm, "%s/%s", newdir, "precision-matrix");
			GMRFLib_bitmap_problem((const char *) fnm, p);
			Free(fnm);
		}
		GMRFLib_sprintf(&fnm, "%s/%s", newdir, "factorisation-information.txt");
		fp = fopen(fnm, "w");
		if (fp) {
			GMRFLib_fact_info_report(fp, &(p->sub_sm_fact));
			fclose(fp);
		}
		GMRFLib_free_problem(p);
		Free(fnm);
	}
	Free(newdir);

	return INLA_OK;
}
int inla_output_graph(inla_tp * mb, const char *dir, GMRFLib_graph_tp * graph)
{
	char *fnm = NULL;
	GMRFLib_sprintf(&fnm, "%s/graph.dat", dir);
	if (mb->verbose) {
		printf("\t\tstore graph in[%s]\n", fnm);
	}

	if (G.binary) {
		GMRFLib_write_graph_binary(fnm, graph);
	} else {
		GMRFLib_write_graph(fnm, graph);
	}

	Free(fnm);

	return INLA_OK;
}
int inla_output_matrix(const char *dir, const char *sdir, const char *filename, int n, double *matrix, int *order)
{
	char *fnm, *ndir;

	if (sdir) {
		GMRFLib_sprintf(&ndir, "%s/%s", dir, sdir);
	} else {
		GMRFLib_sprintf(&ndir, "%s", dir);
	}

	inla_fnmfix(ndir);
	GMRFLib_sprintf(&fnm, "%s/%s", ndir, filename);

	GMRFLib_matrix_tp *M = Calloc(1, GMRFLib_matrix_tp);

	M->nrow = M->ncol = n;
	M->elems = ISQR(n);
	M->A = Calloc(ISQR(n), double);
	if (order == NULL) {
		memcpy(M->A, matrix, ISQR(n) * sizeof(double));
	} else {
		int i, j;
		for (i = 0; i < n; i++) {
			for (j = 0; j < n; j++) {
				M->A[order[i] + order[j] * n] = matrix[i + j * n];
			}
		}
	}

	M->offset = 0L;
	M->whence = SEEK_SET;
	M->tell = -1L;

	GMRFLib_write_fmesher_file(M, fnm, M->offset, M->whence);
	GMRFLib_matrix_free(M);

	Free(fnm);
	Free(ndir);

	return INLA_OK;
}
int inla_output_names(const char *dir, const char *sdir, int n, const char **names, const char *suffix)
{
	FILE *fp;
	char *fnm, *ndir;

	GMRFLib_sprintf(&ndir, "%s/%s", dir, sdir);
	inla_fnmfix(ndir);
	GMRFLib_sprintf(&fnm, "%s/NAMES", ndir);

	int i;
	fp = fopen(fnm, "w");
	for (i = 0; i < n; i++) {
		fprintf(fp, "%s%s\n", names[i], (suffix ? suffix : ""));
	}
	fclose(fp);

	Free(fnm);
	Free(ndir);

	return INLA_OK;
}
int inla_output_size(const char *dir, const char *sdir, int n, int N, int Ntotal, int ngroup, int nrep)
{
	FILE *fp;
	char *fnm, *ndir;

	GMRFLib_sprintf(&ndir, "%s/%s", dir, sdir);
	inla_fnmfix(ndir);
	GMRFLib_sprintf(&fnm, "%s/size.dat", ndir);

	fp = fopen(fnm, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(fnm);
	}
	if (G.binary) {
		I5W(n, (N > 0 ? N : n), (Ntotal > 0 ? Ntotal : n), (ngroup > 0 ? ngroup : 1), (nrep > 0 ? nrep : 1));
	} else {
		fprintf(fp, "%d\n%d\n%d\n%d\n%d\n", n, (N > 0 ? N : n), (Ntotal > 0 ? Ntotal : n), (ngroup > 0 ? ngroup : 1), (nrep > 0 ? nrep : 1));
	}
	fclose(fp);

	Free(fnm);
	Free(ndir);

	return INLA_OK;
}
int inla_output_id_names(const char *dir, const char *sdir, inla_file_contents_tp * fc)
{
	if (!fc) {
		return INLA_OK;
	}

	char *fnm, *ndir;

	GMRFLib_sprintf(&ndir, "%s/%s", dir, sdir);
	inla_fnmfix(ndir);
	GMRFLib_sprintf(&fnm, "%s/id-names.dat", ndir);

	inla_write_file_contents(fnm, fc);

	Free(fnm);
	Free(ndir);

	return INLA_OK;
}
int inla_output(inla_tp * mb)
{
	int n = 0, i, j, *offsets = NULL, len_offsets, local_verbose = 0;

	assert(mb);
	/*
	 * compute the offset for each pack of the results 
	 */
	len_offsets = 1 + mb->nf + mb->nlinear + (mb->lc_derived_only ? 0 : mb->nlc);
	offsets = Calloc(len_offsets, int);

	j = n = 0;
	offsets[j++] = n;
	n += mb->predictor_n + mb->predictor_m;
	for (i = 0; i < mb->nf; i++) {
		offsets[j++] = n;
		n += mb->f_graph[i]->n;
	}
	for (i = 0; i < mb->nlinear; i++) {
		offsets[j++] = n;
		n++;
	}
	if (!mb->lc_derived_only) {
		for (i = 0; i < mb->nlc; i++) {
			offsets[j++] = n;
			n++;
		}
	}
	assert(mb->hgmrfm->graph->n == n);
	assert(j == len_offsets);
	if (mb->verbose) {
		printf("Store results in directory[%s]\n", mb->dir);
	}
	/*
	 * turn off all the info about output-files... 
	 */
	local_verbose = 0;

	/*
	 * workaround for this strange `bug' detected by valgrind. see below for more info
	 * 
	 * ==24889== Conditional jump or move depends on uninitialised value(s) ==24889== at 0x402F085: (within /usr/lib/libgomp.so.1.0.0) ==24889== by 0x4030657:
	 * GOMP_sections_next (in /usr/lib/libgomp.so.1.0.0) ==24889== by 0x805FD42: inla_output.omp_fn.1 (inla.c:4244) ==24889== by 0x805FCC4: inla_output
	 * (inla.c:4360) ==24889== by 0x80644D2: main (inla.c:5237) 
	 */
#pragma omp parallel for private(i)
	for (i = 0; i < 3; i++) {
		if (i == 0) {
			/*
			 * 
			 */
			int offset = offsets[0];

			inla_output_detail(mb->dir, &(mb->density[offset]),
					   (mb->gdensity ? &(mb->gdensity[offset]) : NULL),
					   NULL, mb->predictor_n + mb->predictor_m, 1,
					   mb->predictor_output, mb->predictor_dir, NULL, NULL, NULL, mb->predictor_tag, NULL, local_verbose);
			inla_output_size(mb->dir, mb->predictor_dir, mb->predictor_n, mb->predictor_n, mb->predictor_n + mb->predictor_m, -1,
					 (mb->predictor_m == 0 ? 1 : 2));

			if (mb->predictor_invlinkfunc && mb->predictor_user_scale) {
				char *sdir, *newtag;

				GMRFLib_sprintf(&newtag, "%s in user scale", mb->predictor_tag);
				GMRFLib_sprintf(&sdir, "%s user scale", mb->predictor_dir);
				if (0) {
					// OLD CODE WHICH IS DISABLED
					inla_output_detail(mb->dir, &(mb->density_transform[offset]), NULL, NULL, mb->predictor_n + mb->predictor_m, 1,
							   mb->predictor_output, sdir, NULL, NULL, NULL, newtag, NULL, local_verbose);
				} else {
					inla_output_detail(mb->dir, &(mb->density[offset]),
							   (mb->gdensity ? &(mb->gdensity[offset]) : NULL),
							   NULL, mb->predictor_n + mb->predictor_m, 1,
							   mb->predictor_output, sdir, NULL, NULL, mb->transform_funcs, newtag, NULL, local_verbose);
				}
				inla_output_size(mb->dir, sdir, mb->predictor_n + mb->predictor_m, -1, -1, -1, (mb->predictor_m == 0 ? 1 : 2));
			}
		} else if (i == 1) {
			int ii;

			for (ii = 0; ii < mb->nf; ii++) {
				int offset = offsets[ii + 1];

				inla_output_detail(mb->dir, &(mb->density[offset]),
						   (mb->gdensity ? &(mb->gdensity[offset]) : NULL),
						   mb->f_locations[ii],
						   mb->f_graph[ii]->n, mb->f_nrep[ii] * mb->f_ngroup[ii], mb->f_output[ii], mb->f_dir[ii], NULL, NULL, NULL,
						   mb->f_tag[ii], mb->f_modelname[ii], local_verbose);
				inla_output_size(mb->dir, mb->f_dir[ii], mb->f_n[ii], mb->f_N[ii], mb->f_Ntotal[ii], mb->f_ngroup[ii], mb->f_nrep[ii]);
				inla_output_id_names(mb->dir, mb->f_dir[ii], mb->f_id_names[ii]);
			}
		} else if (i == 2) {
			/*
			 * join these all these 
			 */
			int ii;

			if (1) {
				/*
				 * This the offset 
				 */
				FILE *fp;
				char *fnm;

				GMRFLib_sprintf(&fnm, "%s/totaloffset", mb->dir);
				inla_fnmfix(fnm);
				inla_mkdir(fnm);
				Free(fnm);
				GMRFLib_sprintf(&fnm, "%s/totaloffset/totaloffset.dat", mb->dir);
				inla_fnmfix(fnm);
				fp = fopen(fnm, (G.binary ? "wb" : "w"));
				Free(fnm);
				if (G.binary) {
					for (ii = 0; ii < mb->predictor_n + mb->predictor_m; ii++) {
						DW(OFFSET3(ii));
					}
				} else {
					for (ii = 0; ii < mb->predictor_n + mb->predictor_m; ii++) {
						fprintf(fp, "%1d %.12g\n", ii, OFFSET3(ii));
					}
				}
				fclose(fp);
			}

			for (ii = 0; ii < mb->nlinear; ii++) {
				int offset = offsets[mb->nf + 1 + ii];

				inla_output_detail(mb->dir, &(mb->density[offset]),
						   (mb->gdensity ? &(mb->gdensity[offset]) : NULL),
						   NULL, 1, 1, mb->linear_output[ii], mb->linear_dir[ii], NULL, NULL, NULL, mb->linear_tag[ii], NULL,
						   local_verbose);
				inla_output_size(mb->dir, mb->linear_dir[ii], 1, -1, -1, -1, -1);
			}
			if (!mb->lc_derived_only) {
				/*
				 * is only added if the derived ones as well are there... 
				 */
				if (mb->density_lin) {
					char *newtag2, *newdir2;

					GMRFLib_sprintf(&newtag2, "lincombs.all");
					GMRFLib_sprintf(&newdir2, "lincombs.all");

					ii = 0;
					int offset = offsets[mb->nf + 1 + mb->nlinear + ii];
					inla_output_detail(mb->dir, &(mb->density[offset]),
							   (mb->gdensity ? &(mb->gdensity[offset]) : NULL),
							   mb->lc_order, mb->nlc, 1, mb->lc_output[ii], newdir2, NULL, NULL, NULL, newtag2, NULL, local_verbose);
					inla_output_size(mb->dir, newdir2, mb->nlc, -1, -1, -1, -1);
					inla_output_names(mb->dir, newdir2, mb->nlc, (const char **) ((void *) (mb->lc_tag)), NULL);

					Free(newtag2);
					Free(newdir2);
				}
			}
			if (mb->density_lin) {
				char *newtag2, *newdir2;
				ii = 0;

				GMRFLib_sprintf(&newtag2, "lincombs.derived.all");
				GMRFLib_sprintf(&newdir2, "lincombs.derived.all");
				inla_output_detail(mb->dir, &(mb->density_lin[ii]), &(mb->density_lin[ii]), mb->lc_order, mb->nlc, 1,
						   mb->lc_output[ii], newdir2, NULL, NULL, NULL, newtag2, NULL, local_verbose);
				inla_output_size(mb->dir, newdir2, mb->nlc, -1, -1, -1, -1);
				inla_output_names(mb->dir, newdir2, mb->nlc, (const char **) ((void *) mb->lc_tag), NULL);

				Free(newtag2);
				Free(newdir2);
			}

			if (mb->density_hyper) {

				for (ii = 0; ii < mb->ntheta; ii++) {
					char *sdir;

					GMRFLib_sprintf(&sdir, "hyperparameter 1 %.6d %s", ii, mb->theta_dir[ii]);
					inla_output_detail(mb->dir, &(mb->density_hyper[ii]), NULL, NULL, 1, 1, mb->output, sdir, NULL, NULL, NULL,
							   mb->theta_tag[ii], NULL, local_verbose);
					inla_output_size(mb->dir, sdir, 1, -1, -1, -1, -1);
					GMRFLib_sprintf(&sdir, "hyperparameter 2 %.6d %s user scale", ii, mb->theta_dir[ii]);
					inla_output_detail(mb->dir, &(mb->density_hyper[ii]), NULL, NULL, 1, 1, mb->output, sdir,
							   mb->theta_map[ii], mb->theta_map_arg[ii], NULL, mb->theta_tag_userscale[ii], NULL, local_verbose);
				}
			}

			if (GMRFLib_ai_INLA_userfunc0_density && GMRFLib_ai_INLA_userfunc0_dim > 0) {
				/*
				 * we need to create the corresponding normal as well 
				 */
				char *sdir;
				int dim = GMRFLib_ai_INLA_userfunc0_dim;

				GMRFLib_density_tp **gd = Calloc(dim, GMRFLib_density_tp *);

				for (ii = 0; ii < dim; ii++) {
					GMRFLib_density_create_normal(&(gd[ii]), 0.0, 1.0,
								      GMRFLib_ai_INLA_userfunc0_density[ii]->user_mean,
								      GMRFLib_ai_INLA_userfunc0_density[ii]->user_stdev);
				}
				sdir = GMRFLib_strdup("random.effect.UserFunction0");
				inla_output_detail(mb->dir, GMRFLib_ai_INLA_userfunc0_density, gd, NULL, GMRFLib_ai_INLA_userfunc0_dim, 1,
						   mb->output, sdir, NULL, NULL, NULL, "UserFunction0", NULL, local_verbose);
				inla_output_size(mb->dir, sdir, GMRFLib_ai_INLA_userfunc0_dim, -1, -1, -1, -1);
				Free(sdir);
				for (ii = 0; ii < dim; ii++)
					GMRFLib_free_density(gd[ii]);
				Free(gd);
			}
			if (GMRFLib_ai_INLA_userfunc1_density && GMRFLib_ai_INLA_userfunc1_dim > 0) {
				/*
				 * we need to create the corresponding normal as well 
				 */
				char *sdir;
				int dim = GMRFLib_ai_INLA_userfunc1_dim;
				GMRFLib_density_tp **gd = Calloc(dim, GMRFLib_density_tp *);

				for (ii = 0; ii < dim; ii++) {
					GMRFLib_density_create_normal(&(gd[ii]), 0.0, 1.0,
								      GMRFLib_ai_INLA_userfunc1_density[ii]->user_mean,
								      GMRFLib_ai_INLA_userfunc1_density[ii]->user_stdev);
				}
				sdir = GMRFLib_strdup("random.effect.UserFunction1");
				inla_output_detail(mb->dir, GMRFLib_ai_INLA_userfunc1_density, gd, NULL, GMRFLib_ai_INLA_userfunc1_dim, 1,
						   mb->output, sdir, NULL, NULL, NULL, "UserFunction1", NULL, local_verbose);
				inla_output_size(mb->dir, sdir, GMRFLib_ai_INLA_userfunc1_dim, -1, -1, -1, -1);

				Free(sdir);
				for (ii = 0; ii < dim; ii++)
					GMRFLib_free_density(gd[ii]);
				Free(gd);
			}
			if (GMRFLib_ai_INLA_userfunc2_density && GMRFLib_ai_INLA_userfunc2_n > 0) {
				for (ii = 0; ii < GMRFLib_ai_INLA_userfunc2_n; ii++) {
					/*
					 * we need to create the corresponding normal as well 
					 */
					char *sdir, *local_tag;

					int dim = GMRFLib_ai_INLA_userfunc2_len[ii];
					GMRFLib_density_tp **gd = Calloc(dim, GMRFLib_density_tp *);

					int jj;
					for (jj = 0; jj < dim; jj++) {
						GMRFLib_density_create_normal(&(gd[jj]), 0.0, 1.0,
									      GMRFLib_ai_INLA_userfunc2_density[ii][jj]->user_mean,
									      GMRFLib_ai_INLA_userfunc2_density[ii][jj]->user_stdev);
					}
					GMRFLib_sprintf(&sdir, "spde2.blc.%6.6d", ii + 1);
					GMRFLib_sprintf(&local_tag, "%s", GMRFLib_ai_INLA_userfunc2_tag[ii]);
					inla_output_detail(mb->dir, GMRFLib_ai_INLA_userfunc2_density[ii], gd, NULL, dim, 1,
							   mb->output, sdir, NULL, NULL, NULL, local_tag, NULL, local_verbose);
					inla_output_size(mb->dir, sdir, dim, -1, -1, -1, -1);

					Free(sdir);
					Free(local_tag);
					for (jj = 0; jj < dim; jj++) {
						GMRFLib_free_density(gd[jj]);
					}
					Free(gd);
				}
			}
			if (GMRFLib_ai_INLA_userfunc3_density && GMRFLib_ai_INLA_userfunc3_n > 0) {
				for (ii = 0; ii < GMRFLib_ai_INLA_userfunc3_n; ii++) {
					/*
					 * we need to create the corresponding normal as well 
					 */
					char *sdir, *local_tag;

					int dim = GMRFLib_ai_INLA_userfunc3_len[ii];
					GMRFLib_density_tp **gd = Calloc(dim, GMRFLib_density_tp *);

					int jj;
					for (jj = 0; jj < dim; jj++) {
						GMRFLib_density_create_normal(&(gd[jj]), 0.0, 1.0,
									      GMRFLib_ai_INLA_userfunc3_density[ii][jj]->user_mean,
									      GMRFLib_ai_INLA_userfunc3_density[ii][jj]->user_stdev);
					}
					GMRFLib_sprintf(&sdir, "spde3.blc.%6.6d", ii + 1);
					GMRFLib_sprintf(&local_tag, "%s", GMRFLib_ai_INLA_userfunc3_tag[ii]);
					inla_output_detail(mb->dir, GMRFLib_ai_INLA_userfunc3_density[ii], gd, NULL, dim, 1,
							   mb->output, sdir, NULL, NULL, NULL, local_tag, NULL, local_verbose);
					inla_output_size(mb->dir, sdir, dim, -1, -1, -1, -1);

					Free(sdir);
					Free(local_tag);
					for (jj = 0; jj < dim; jj++) {
						GMRFLib_free_density(gd[jj]);
					}
					Free(gd);
				}
			}

			if (mb->misc_output) {
				inla_output_misc(mb->dir, mb->misc_output, mb->ntheta, mb->theta_tag, mb->theta_from, mb->theta_to, mb->lc_order, local_verbose,
						 mb);
			}
			if (mb->cpo) {
				inla_output_detail_cpo(mb->dir, mb->cpo, mb->predictor_ndata, local_verbose);
			}
			if (mb->po) {
				inla_output_detail_po(mb->dir, mb->po, mb->predictor_ndata, local_verbose);
			}
			if (mb->dic) {
				inla_output_detail_dic(mb->dir, mb->dic, mb->family_idx, mb->len_family_idx, local_verbose);
			}
			if (mb->output->mlik) {
				inla_output_detail_mlik(mb->dir, &(mb->mlik), local_verbose);
			}
			inla_output_detail_neffp(mb->dir, &(mb->neffp), local_verbose);
			inla_output_detail_x(mb->dir, mb->x_file, mb->nx_file);
			inla_output_detail_theta(mb->dir, mb->theta, mb->ntheta);
			inla_output_hgid(mb->dir);
			inla_output_linkfunctions(mb->dir, mb);
			if ((!mb->reuse_mode) || (mb->reuse_mode && mb->reuse_mode_but_restart)) {
				// disable output theta-mode to file '.inla-mode'
				// inla_output_detail_theta_sha1(mb->sha1_hash, mb->theta, mb->ntheta);
			}

			if (mb->output->q) {
				if (local_verbose == 0) {
					int save = mb->verbose;
					mb->verbose = 0;
					inla_output_Q(mb, mb->dir, mb->hgmrfm->graph);
					mb->verbose = save;
				}
			}

			if (mb->output->graph) {
				inla_output_graph(mb, mb->dir, mb->hgmrfm->graph);
			}
		}
	}
	int N = ((GMRFLib_hgmrfm_arg_tp *) mb->hgmrfm->Qfunc_arg)->N;


	/*
	 * workaround for some strange bug deteced by valgrind. ``parallel sections'' give
	 * 
	 * ==24476== Conditional jump or move depends on uninitialised value(s) ==24476== at 0x402F085: (within /usr/lib/libgomp.so.1.0.0) ==24476== by 0x4030657:
	 * GOMP_sections_next (in /usr/lib/libgomp.so.1.0.0) ==24476== by 0x805FD4A: inla_output.omp_fn.1 (inla.c:4244) ==24476== by 0x805FCC4: inla_output
	 * (inla.c:4353) ==24476== by 0x8064485: main (inla.c:5230)
	 * 
	 * wheras the ``parallel for'' is ok. 
	 */
#pragma omp parallel for private(i)
	for (i = 0; i < 2; i++) {
		if (i == 0 && mb->density) {
			int ii;

			for (ii = 0; ii < N; ii++) {
				GMRFLib_free_density(mb->density[ii]);
			}
			Free(mb->density);
		}
		if (i == 1 && mb->gdensity) {
			int ii;

			for (ii = 0; ii < N; ii++) {
				GMRFLib_free_density(mb->gdensity[ii]);
			}
			Free(mb->gdensity);
		}
	}

	return INLA_OK;
}
int inla_output_detail_cpo(const char *dir, GMRFLib_ai_cpo_tp * cpo, int predictor_n, int verbose)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;
	int i, n, add_empty = 1;

	if (!cpo) {
		return INLA_OK;
	}
	// n = cpo->n;
	n = predictor_n;				       /* the CPO and PIT are at the first predictor_n */

	GMRFLib_sprintf(&ndir, "%s/%s", dir, "cpo");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "cpo.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore cpo-results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		IW(predictor_n);
	}
	for (i = 0; i < n; i++) {
		if (cpo->value[i]) {
			if (G.binary) {
				IDW(i, cpo->value[i][0]);
			} else {
				fprintf(fp, "%1d %.8g\n", i, cpo->value[i][0]);
			}
		} else {
			if (add_empty) {
				if (G.binary) {
					IDW(i, NAN);
				} else {
					fprintf(fp, "%1d %.8g\n", i, NAN);
				}
			}
		}
	}
	fclose(fp);
	if (cpo->pit_value) {
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "pit.dat");
		inla_fnmfix(nndir);
		fp = fopen(nndir, (G.binary ? "wb" : "w"));
		if (!fp) {
			inla_error_open_file(nndir);
		}
		if (verbose) {
#pragma omp critical
			{
				printf("\t\tstore pit-results in[%s]\n", nndir);
			}
		}
		if (G.binary) {
			IW(predictor_n);
		}
		for (i = 0; i < n; i++) {
			if (cpo->pit_value[i]) {
				if (G.binary) {
					IDW(i, cpo->pit_value[i][0]);
				} else {
					fprintf(fp, "%1d %.8g\n", i, cpo->pit_value[i][0]);
				}
			} else {
				if (add_empty) {
					if (G.binary) {
						IDW(i, NAN);
					} else {
						fprintf(fp, "%1d %.8g\n", i, NAN);
					}
				}
			}
		}
		fclose(fp);
	}
	if (cpo->failure) {
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "failure.dat");
		inla_fnmfix(nndir);
		fp = fopen(nndir, (G.binary ? "wb" : "w"));
		if (!fp) {
			inla_error_open_file(nndir);
		}
		if (verbose) {
#pragma omp critical
			{
				printf("\t\tstore failure-results in[%s]\n", nndir);
			}
		}
		IW(predictor_n);
		for (i = 0; i < n; i++) {
			if (cpo->failure[i]) {
				if (G.binary) {
					IDW(i, cpo->failure[i][0]);
				} else {
					fprintf(fp, "%1d %.8g\n", i, cpo->failure[i][0]);
				}
			} else {
				if (add_empty) {
					if (G.binary) {
						IDW(i, NAN);
					} else {
						fprintf(fp, "%1d %.8g\n", i, NAN);
					}
				}
			}
		}
		fclose(fp);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "summary.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore summary of cpo-results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		D2W(cpo->mean_value, cpo->gmean_value);
	} else {
		fprintf(fp, "mean value: %g\ngeometric mean value: %g\n", cpo->mean_value, cpo->gmean_value);
	}
	fclose(fp);
	Free(ndir);
	Free(nndir);
	return INLA_OK;
}
int inla_output_detail_po(const char *dir, GMRFLib_ai_po_tp * po, int predictor_n, int verbose)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;
	int i, n, add_empty = 1;

	if (!po) {
		return INLA_OK;
	}
	n = predictor_n;				       /* the PO are at the first predictor_n */

	GMRFLib_sprintf(&ndir, "%s/%s", dir, "po");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "po.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore po-results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		IW(predictor_n);
	}
	for (i = 0; i < n; i++) {
		if (po->value[i]) {
			if (G.binary) {
				ID2W(i, po->value[i][0], po->value[i][1]);
			} else {
				fprintf(fp, "%1d %.8g %.8g\n", i, po->value[i][0], po->value[i][1]);
			}
		} else {
			if (add_empty) {
				if (G.binary) {
					ID2W(i, NAN, NAN);
				} else {
					fprintf(fp, "%1d %.8g %.8g\n", i, NAN, NAN);
				}
			}
		}
	}
	fclose(fp);
	Free(ndir);
	Free(nndir);
	return INLA_OK;
}
int inla_output_detail_dic(const char *dir, GMRFLib_ai_dic_tp * dic, double *family_idx, int len_family_idx, int verbose)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;
	double *tmp = NULL;

#define PAD_WITH_NA(xx)							\
	if (1) {							\
		tmp = Calloc(len_family_idx, double);			\
		memcpy(tmp, xx, dic->n_deviance*sizeof(double));	\
		int i;							\
		for(i = dic->n_deviance; i < len_family_idx; i++){	\
			tmp[i] = NAN;					\
		}							\
	}

	if (!dic) {
		return INLA_OK;
	}
	GMRFLib_sprintf(&ndir, "%s/%s", dir, "dic");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "dic.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore dic-results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		D4W(dic->mean_of_deviance, dic->deviance_of_mean, dic->p, dic->dic);
	} else {
		fprintf(fp, "mean of the deviance: %g\n", dic->mean_of_deviance);
		fprintf(fp, "deviance of the mean: %g\n", dic->deviance_of_mean);
		fprintf(fp, "effective number of parameters: %g\n", dic->p);
		fprintf(fp, "dic: %g\n", dic->dic);
	}
	fclose(fp);

	if (dic->n_deviance > 0) {
		GMRFLib_matrix_tp *M = NULL;

		M = Calloc(1, GMRFLib_matrix_tp);
		M->nrow = len_family_idx;
		M->ncol = 1;
		M->elems = M->nrow * M->ncol;

		PAD_WITH_NA(dic->e_deviance);
		M->A = tmp;
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "e_deviance.dat");
		inla_fnmfix(nndir);
		GMRFLib_write_fmesher_file(M, nndir, (long int) 0, -1);
		Free(tmp);

		PAD_WITH_NA(dic->deviance_e);
		M->A = tmp;
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "deviance_e.dat");
		inla_fnmfix(nndir);
		GMRFLib_write_fmesher_file(M, nndir, (long int) 0, -1);
		Free(tmp);

		M->A = family_idx;
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "family_idx.dat");
		inla_fnmfix(nndir);
		GMRFLib_write_fmesher_file(M, nndir, (long int) 0, -1);

		M->A = NULL;
		GMRFLib_matrix_free(M);
	}

	Free(ndir);
	Free(nndir);
#undef PAD_WITH_NA
	return INLA_OK;
}
int inla_output_misc(const char *dir, GMRFLib_ai_misc_output_tp * mo, int ntheta, char **theta_tag, char **theta_from, char **theta_to,
		     double *lc_order, int verbose, inla_tp * mb)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL, *nnndir = NULL;
	FILE *fp = NULL;
	int i, j, any;

	if (!mo) {
		return INLA_OK;
	}
	GMRFLib_sprintf(&ndir, "%s/%s", dir, "misc");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}

	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore misc-output in[%s]\n", ndir);
		}
	}

	GMRFLib_sprintf(&nndir, "%s/theta-tags", ndir);
	fp = fopen(nndir, "w");
	for (i = 0; i < ntheta; i++) {
		fprintf(fp, "%s\n", theta_tag[i]);
	}
	fclose(fp);

	for (i = any = 0; i < ntheta; i++) {
		any = (any || theta_from[i]);
	}
	if (any) {
		GMRFLib_sprintf(&nndir, "%s/theta-from", ndir);
		fp = fopen(nndir, "w");
		for (i = 0; i < ntheta; i++) {
			fprintf(fp, "%s\n", theta_from[i]);
		}
		fclose(fp);
	}

	for (i = any = 0; i < ntheta; i++) {
		any = (any || theta_to[i]);
	}
	if (any) {
		GMRFLib_sprintf(&nndir, "%s/theta-to", ndir);
		fp = fopen(nndir, "w");
		for (i = 0; i < ntheta; i++) {
			fprintf(fp, "%s\n", theta_to[i]);
		}
		fclose(fp);
	}

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "covmat-hyper-internal.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		DW(mo->nhyper);
		for (i = 0; i < ISQR(mo->nhyper); i++) {
			DW(mo->cov_m[i]);
		}
	} else {
		fprintf(fp, "%d\n", mo->nhyper);
		for (i = 0; i < mo->nhyper; i++) {
			for (j = 0; j < mo->nhyper; j++) {
				fprintf(fp, " %.12g", mo->cov_m[i + j * mo->nhyper]);
			}
			fprintf(fp, "\n");
		}
	}
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "covmat-eigenvectors.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		DW(mo->nhyper);
		for (i = 0; i < ISQR(mo->nhyper); i++) {
			DW(mo->eigenvectors[i]);
		}
	} else {
		fprintf(fp, "%d\n", mo->nhyper);
		for (i = 0; i < mo->nhyper; i++) {
			for (j = 0; j < mo->nhyper; j++) {
				fprintf(fp, " %.12g", mo->eigenvectors[i + j * mo->nhyper]);
			}
			fprintf(fp, "\n");
		}
	}
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "covmat-eigenvalues.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		DW(mo->nhyper);
		for (i = 0; i < mo->nhyper; i++) {
			DW(mo->eigenvalues[i]);
		}
	} else {
		fprintf(fp, "%d\n", mo->nhyper);
		for (i = 0; i < mo->nhyper; i++) {
			fprintf(fp, " %.12g\n", mo->eigenvalues[i]);
		}
	}
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "reordering.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		for (i = 0; i < mo->len_reordering; i++) {
			IW(mo->reordering[i] + 1);	       /* yes, use 1-based indexing. */
		}
	} else {
		fprintf(fp, "%d\n", mo->len_reordering);
		for (i = 0; i < mo->len_reordering; i++) {
			fprintf(fp, " %d %d\n", i, mo->reordering[i]);
		}
	}
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "mode-status.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "w");
	if (!fp) {
		inla_error_open_file(nndir);
	}
	fprintf(fp, "%1d\n", mo->mode_status);
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "log-posterior-mode.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "w");
	if (!fp) {
		inla_error_open_file(nndir);
	}
	fprintf(fp, "%.16f\n", mo->log_posterior_mode);
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "stdev_corr_pos.dat");
	inla_fnmfix(nndir);
	if (mo->stdev_corr_pos) {
		GMRFLib_matrix_tp *M = Calloc(1, GMRFLib_matrix_tp);
		M->nrow = mo->nhyper;
		M->ncol = 1;
		M->elems = M->nrow * M->ncol;
		M->A = Calloc(mo->nhyper, double);
		memcpy(M->A, mo->stdev_corr_pos, mo->nhyper * sizeof(double));
		GMRFLib_write_fmesher_file(M, nndir, 0L, -1);
		GMRFLib_matrix_free(M);
	}

	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "stdev_corr_neg.dat");
	inla_fnmfix(nndir);
	if (mo->stdev_corr_pos) {
		GMRFLib_matrix_tp *M = Calloc(1, GMRFLib_matrix_tp);
		M->nrow = mo->nhyper;
		M->ncol = 1;
		M->elems = M->nrow * M->ncol;
		M->A = Calloc(mo->nhyper, double);
		memcpy(M->A, mo->stdev_corr_neg, mo->nhyper * sizeof(double));
		GMRFLib_write_fmesher_file(M, nndir, 0L, -1);
		GMRFLib_matrix_free(M);
	}

	if (mo->compute_corr_lin && mo->corr_lin) {
		/*
		 * OOPS: this matrix is in its own internal ordering, so we need to fix it here.
		 */
		assert(lc_order);
		int n = mo->compute_corr_lin;
		int *order = Calloc(n, int);
		int i;

		for (i = 0; i < n; i++) {
			order[i] = (int) lc_order[i] - 1;
		}
		assert(GMRFLib_imin_value(order, n, NULL) == 0);
		assert(GMRFLib_imax_value(order, n, NULL) == n - 1);
		inla_output_matrix(ndir, NULL, "lincomb_derived_correlation_matrix.dat", n, mo->corr_lin, order);
		inla_output_matrix(ndir, NULL, "lincomb_derived_covariance_matrix.dat", n, mo->cov_lin, order);
		Free(order);
	}

	if (mo->configs) {

		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "config");
		inla_fnmfix(nndir);
		if (inla_mkdir(nndir) != 0) {
			GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", nndir, strerror(errno));
			inla_error_general(msg);
		}

		GMRFLib_sprintf(&nnndir, "%s/%s", nndir, "theta-tag.dat");
		inla_fnmfix(nnndir);
		fp = fopen(nnndir, "w");
		for (i = 0; i < mb->ntheta; i++) {
			fprintf(fp, "%s\n", mb->theta_tag[i]);
		}
		fclose(fp);

		GMRFLib_sprintf(&nnndir, "%s/%s", nndir, "tag.dat");
		inla_fnmfix(nnndir);
		fp = fopen(nnndir, "w");
		for (i = 0; i < mb->idx_tot; i++) {
			fprintf(fp, "%s\n", mb->idx_tag[i]);
		}
		fclose(fp);

		GMRFLib_sprintf(&nnndir, "%s/%s", nndir, "start.dat");
		inla_fnmfix(nnndir);
		fp = fopen(nnndir, "w");
		for (i = 0; i < mb->idx_tot; i++) {
			fprintf(fp, "%d\n", mb->idx_start[i]);
		}
		fclose(fp);

		GMRFLib_sprintf(&nnndir, "%s/%s", nndir, "n.dat");
		inla_fnmfix(nnndir);
		fp = fopen(nnndir, "w");
		for (i = 0; i < mb->idx_tot; i++) {
			fprintf(fp, "%d\n", mb->idx_n[i]);
		}
		fclose(fp);

		GMRFLib_sprintf(&nnndir, "%s/%s", nndir, "configs.dat");
		inla_fnmfix(nnndir);
		fp = fopen(nnndir, "wb");

		int id, header = 0, nconfig = 0;

		for (id = 0; id < GMRFLib_MAX_THREADS; id++) {
			if (mo->configs[id]) {
				nconfig += mo->configs[id]->nconfig;	/* need the accumulated one! */
			}
		}

		for (id = 0; id < GMRFLib_MAX_THREADS; id++) {
			if (!header) {
				header = 1;		       /* do this only once */
				fwrite((void *) &(mo->configs[id]->n), sizeof(int), (size_t) 1, fp);
				fwrite((void *) &(mo->configs[id]->nz), sizeof(int), (size_t) 1, fp);
				fwrite((void *) &(mo->configs[id]->ntheta), sizeof(int), (size_t) 1, fp);
				fwrite((void *) mo->configs[id]->i, sizeof(int), (size_t) mo->configs[id]->nz, fp);	/* 0-based! */
				fwrite((void *) mo->configs[id]->j, sizeof(int), (size_t) mo->configs[id]->nz, fp);	/* 0-based! */
				fwrite((void *) &nconfig, sizeof(int), (size_t) 1, fp);	/* yes!!! */

				if (mo->configs[id]->constr) {
					fwrite((void *) &(mo->configs[id]->constr->nc), sizeof(int), (size_t) 1, fp);
					fwrite((void *) mo->configs[id]->constr->a_matrix, sizeof(double),
					       (size_t) (mo->configs[id]->n * mo->configs[id]->constr->nc), fp);
					fwrite((void *) mo->configs[id]->constr->e_vector, sizeof(double), (size_t) mo->configs[id]->constr->nc, fp);
				} else {
					int zero = 0;
					fwrite((void *) &zero, sizeof(int), (size_t) 1, fp);
				}
			}

			if (mo->configs[id]) {
				double *off = Calloc(mo->configs[id]->n, double);
				memcpy(off, &(OFFSET3(0)), (mb->predictor_n + mb->predictor_m) * sizeof(double));

				for (i = 0; i < mo->configs[id]->nconfig; i++) {
					fwrite((void *) &(mo->configs[id]->config[i]->log_posterior), sizeof(double), (size_t) 1, fp);
					fwrite((void *) mo->configs[id]->config[i]->theta, sizeof(double), (size_t) mo->configs[id]->ntheta, fp);
					fwrite((void *) mo->configs[id]->config[i]->mean, sizeof(double), (size_t) mo->configs[id]->n, fp);
					fwrite((void *) mo->configs[id]->config[i]->improved_mean, sizeof(double), (size_t) mo->configs[id]->n, fp);
					fwrite((void *) mo->configs[id]->config[i]->skewness, sizeof(double), (size_t) mo->configs[id]->n, fp);
					fwrite((void *) off, sizeof(double), (size_t) mo->configs[id]->n, fp);	// To be added later.
					fwrite((void *) mo->configs[id]->config[i]->Q, sizeof(double), (size_t) mo->configs[id]->nz, fp);
					fwrite((void *) mo->configs[id]->config[i]->Qinv, sizeof(double), (size_t) mo->configs[id]->nz, fp);
				}

				Free(off);
			}
		}
		fclose(fp);
	}

	return INLA_OK;
}
int inla_output_detail_mlik(const char *dir, GMRFLib_ai_marginal_likelihood_tp * mlik, int verbose)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;

	if (!mlik) {
		return INLA_OK;
	}
	GMRFLib_sprintf(&ndir, "%s/%s", dir, "marginal-likelihood");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "marginal-likelihood.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore marginal-likelihood results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		D2W(mlik->marginal_likelihood_integration, mlik->marginal_likelihood_gaussian_approx);
	} else {
		fprintf(fp, "log marginal-likelihood (integration): %g\n", mlik->marginal_likelihood_integration);
		fprintf(fp, "log marginal-likelihood (Gaussian): %g\n", mlik->marginal_likelihood_gaussian_approx);
	}
	fclose(fp);
	Free(ndir);
	Free(nndir);
	return INLA_OK;
}

int inla_output_detail_neffp(const char *dir, GMRFLib_ai_neffp_tp * neffp, int verbose)
{
	/*
	 * output whatever is requested.... 
	 */
	char *ndir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;

	if (!neffp) {
		return INLA_OK;
	}
	GMRFLib_sprintf(&ndir, "%s/%s", dir, "neffp");
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	GMRFLib_sprintf(&nndir, "%s/%s", ndir, "neffp.dat");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (verbose) {
#pragma omp critical
		{
			printf("\t\tstore neffp results in[%s]\n", nndir);
		}
	}
	if (G.binary) {
		D3W(neffp->mean, neffp->stdev, neffp->nrep);
	} else {
		fprintf(fp, "Expectected  number of parameters: %g \n", neffp->mean);
		fprintf(fp, "Stdev of the number of parameters: %g \n", neffp->stdev);
		fprintf(fp, "Number of equivalent replicates  : %g \n", neffp->nrep);
	}
	fclose(fp);
	Free(ndir);
	Free(nndir);
	return INLA_OK;
}
int inla_output_hgid(const char *dir)
{
	char *nndir = NULL;

	FILE *fp = NULL;

	GMRFLib_sprintf(&nndir, "%s/%s", dir, ".hgid");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "w");
	if (!fp) {
		inla_error_open_file(nndir);
	}
	fprintf(fp, "%s\n", RCSId);
	fclose(fp);
	Free(nndir);

	return INLA_OK;
}
int inla_output_linkfunctions(const char *dir, inla_tp * mb)
{
	char *nndir = NULL;

	FILE *fp = NULL;

	GMRFLib_sprintf(&nndir, "%s/%s", dir, "linkfunctions.names");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "w");
	if (!fp) {
		inla_error_open_file(nndir);
	}

	int i, j;

	for (j = 0; j < mb->nds; j++) {
		link_func_tp *lf = mb->data_sections[j].predictor_invlinkfunc;

		if (lf == link_probit) {
			fprintf(fp, "probit\n");
		} else if (lf == link_tan) {
			fprintf(fp, "tan\n");
		} else if (lf == link_cloglog) {
			fprintf(fp, "cloglog\n");
		} else if (lf == link_log) {
			fprintf(fp, "log\n");
		} else if (lf == link_logit) {
			fprintf(fp, "logit\n");
		} else if (lf == link_identity) {
			fprintf(fp, "identity\n");
		} else if (lf == link_sslogit) {
			fprintf(fp, "sslogit\n");
		} else if (lf == link_logoffset) {
			fprintf(fp, "logoffset\n");
		} else if (lf == NULL) {
			fprintf(fp, "invalid\n");
		} else {
			fprintf(fp, "invalid\n");
		}
	}
	fclose(fp);
	Free(nndir);

	GMRFLib_sprintf(&nndir, "%s/%s", dir, "linkfunctions.link");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "wb");
	if (!fp) {
		inla_error_open_file(nndir);
	}

	/*
	 * need to use double as we need NAN
	 */
	double *idx = Calloc(mb->predictor_ndata, double);
	for (i = 0; i < mb->predictor_ndata; i++) {
		int found;

		idx[i] = NAN;
		for (j = found = 0; j < mb->nds && !found; j++) {
			if (mb->data_sections[j].predictor_invlinkfunc == mb->predictor_invlinkfunc[i]) {
				found = 1;
				idx[i] = j;
			}
		}
	}

	fwrite((void *) &(mb->predictor_ndata), sizeof(int), 1, fp);
	fwrite((void *) idx, sizeof(double), (size_t) mb->predictor_ndata, fp);
	Free(idx);

	fclose(fp);
	Free(nndir);

	return INLA_OK;
}
int inla_output_ok(const char *dir)
{
	char *nndir = NULL;

	FILE *fp = NULL;

	GMRFLib_sprintf(&nndir, "%s/%s", dir, ".ok");
	inla_fnmfix(nndir);
	fp = fopen(nndir, "w");
	if (!fp) {
		inla_error_open_file(nndir);
	}
	fprintf(fp, "1");
	fclose(fp);
	Free(nndir);

	return INLA_OK;
}
int inla_output_detail_theta(const char *dir, double ***theta, int n_theta)
{
	/*
	 * write the mode of theta to the file DIR/.theta_mode. This is used only internally... 
	 */
	int i;
	char *nndir = NULL;

	FILE *fp = NULL;

	GMRFLib_sprintf(&nndir, "%s/%s", dir, ".theta_mode");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		IW(n_theta);
		for (i = 0; i < n_theta; i++) {
			DW(theta[i][0][0]);
		}
	} else {
		for (i = 0; i < n_theta; i++) {
			fprintf(fp, "%.12f\n", theta[i][0][0]);
		}
	}
	fclose(fp);
	Free(nndir);
	return INLA_OK;
}
int inla_output_detail_x(const char *dir, double *x, int n_x)
{
	/*
	 * write the mode of x to the file DIR/.x_mode. This is used only internally... 
	 */
	int i;
	char *nndir = NULL;

	FILE *fp = NULL;

	GMRFLib_sprintf(&nndir, "%s/%s", dir, ".x_mode");
	inla_fnmfix(nndir);
	fp = fopen(nndir, (G.binary ? "wb" : "w"));
	if (!fp) {
		inla_error_open_file(nndir);
	}
	if (G.binary) {
		IW(n_x);
		for (i = 0; i < n_x; i++) {
			DW(x[i]);
		}
	} else {
		for (i = 0; i < n_x; i++) {
			fprintf(fp, "%.12f\n", x[i]);
		}
	}
	fclose(fp);
	Free(nndir);
	return INLA_OK;
}
int inla_output_detail_theta_sha1(unsigned char *sha1_hash, double ***theta, int n_theta)
{
	int i;
	FILE *fp;

	assert(sha1_hash);
	fp = fopen(MODEFILENAME, "w");

	if (!fp) {
		return INLA_FAIL;
	}
	inla_print_sha1(fp, sha1_hash);
	fprintf(fp, "%d\n", n_theta);
	for (i = 0; i < n_theta; i++) {
		fprintf(fp, "%.12f\n", theta[i][0][0]);
	}
	fclose(fp);

	return INLA_OK;
}
int inla_read_theta_sha1(unsigned char **sha1_hash, double **theta, int *ntheta)
{
#define EXIT_READ_FAIL				\
	if (1) {				\
		Free(hash);			\
		*sha1_hash = NULL;		\
		*theta = NULL;			\
		*ntheta = 0;			\
		return INLA_OK;			\
	}


	unsigned char *hash = NULL;
	int debug = 0, i;
	FILE *fp;

	fp = fopen(MODEFILENAME, "r");
	if (!fp) {
		EXIT_READ_FAIL;
	} else {
		hash = Calloc(SHA_DIGEST_LENGTH + 1, unsigned char);
		for (i = 0; i < SHA_DIGEST_LENGTH; i++) {
			unsigned int ui;
			if (fscanf(fp, MODEFILENAME_FMT, &ui) == EOF) {
				EXIT_READ_FAIL;
			}
			hash[i] = (unsigned char) ui;
		}
		hash[SHA_DIGEST_LENGTH] = '\0';
		if (debug) {
			inla_print_sha1(stdout, hash);
		}
		if (fscanf(fp, "%d\n", ntheta) == EOF) {
			EXIT_READ_FAIL;
		}
		if (debug) {
			printf("ntheta %d\n", *ntheta);
		}
		if (*ntheta > 0) {
			theta[0] = Calloc(*ntheta, double);
			for (i = 0; i < *ntheta; i++) {
				if (fscanf(fp, "%lf\n", &(theta[0][i])) == EOF) {
					EXIT_READ_FAIL;
				}
				if (debug) {
					printf("theta[%1d] = %.12f\n", i, theta[0][i]);
				}
			}
		} else {
			*theta = NULL;
		}
		*sha1_hash = hash;
		fclose(fp);
	}
	return INLA_OK;
#undef EXIT_READ_FAIL
}
int inla_integrate_func(double *d_mean, double *d_stdev, double *d_mode, GMRFLib_density_tp * density, map_func_tp * func, void *func_arg,
			GMRFLib_transform_array_func_tp * tfunc)
{
	/*
	 * We need to integrate to get the transformed mean and variance. Use a simple Simpsons-rule.  The simple mapping we did before was not good enough,
	 * obviously... 
	 */
	if (!func && !tfunc) {
		*d_mean = density->user_mean;
		*d_stdev = density->user_stdev;
		*d_mode = density->user_mode;

		return GMRFLib_SUCCESS;
	}
#define MAP_X(_x_user) (func ? func(_x_user, MAP_FORWARD, func_arg) : \
			(tfunc ? tfunc->func(_x_user, GMRFLib_TRANSFORM_FORWARD, tfunc->arg, tfunc->cov) : \
			 (_x_user)))
#define MAP_STDEV(_stdev_user, _mean_user) (func ? (_stdev_user)*ABS(func(_mean_user, MAP_DFORWARD, func_arg)) : \
	(tfunc ? (_stdev_user)*ABS(tfunc->func(_mean_user, GMRFLib_TRANSFORM_DFORWARD, tfunc->arg, tfunc->cov)) : \
	 (_stdev_user)))

#define TRANSFORMED_LOGDENS(_x, _logdens) (func ? ((_logdens) - log(ABS(func(_x, MAP_DFORWARD, func_arg)))) : \
					   (tfunc ? ((_logdens) - log(ABS(tfunc->func(_x, GMRFLib_TRANSFORM_DFORWARD, tfunc->arg, tfunc->cov)))) : \
				       (_logdens)))

	int i, k, debug = 0;
	int npm = 4 * GMRFLib_faster_integration_np;
	double dxx, d, xx_local, fval;
	double w[2] = { 4.0, 2.0 };
	double sum[3] = { 0.0, 0.0, 0.0 };
	double *xpm, *ldm, *work;

	double tlogdens, tlogdens_max = NAN;
	int idx_tlogdens_max = -1;

	work = Calloc(2 * npm, double);
	xpm = work;
	ldm = work + npm;

	dxx = (density->x_max - density->x_min) / (npm - 1.0);
	xpm[0] = density->x_min;
	for (i = 1; i < npm; i++) {
		xpm[i] = xpm[0] + i * dxx;
	}
	GMRFLib_evaluate_nlogdensity(ldm, xpm, npm, density);

	sum[0] = exp(ldm[0]) + exp(ldm[npm - 1]);
	xx_local = GMRFLib_density_std2user(xpm[0], density);
	sum[1] = exp(ldm[0]) * MAP_X(xx_local);
	sum[2] = exp(ldm[0]) * SQR(MAP_X(xx_local));
	xx_local = GMRFLib_density_std2user(xpm[npm - 1], density);
	sum[1] += exp(ldm[npm - 1]) * MAP_X(xx_local);
	sum[2] += exp(ldm[npm - 1]) * SQR(MAP_X(xx_local));

	for (i = 1, k = 0; i < npm - 1; i++, k = (k + 1) % 2) {
		d = exp(ldm[i]);
		xx_local = GMRFLib_density_std2user(xpm[i], density);
		fval = MAP_X(xx_local);

		sum[0] += d * w[k];
		sum[1] += d * w[k] * fval;
		sum[2] += d * w[k] * SQR(fval);

		/*
		 * extract the mode
		 */
		tlogdens = TRANSFORMED_LOGDENS(xx_local, ldm[i]);
		if (idx_tlogdens_max < 0 || tlogdens > tlogdens_max) {
			idx_tlogdens_max = i;
			tlogdens_max = tlogdens;
		}
	}
	sum[0] *= dxx / 3.0;				       /* this should be 1.0 */
	sum[1] *= dxx / 3.0;
	sum[2] *= dxx / 3.0;

	if (debug) {
		printf("NEW intergral %g\n", sum[0]);
		printf("NEW mean %g simple %g\n", sum[1] / sum[0], MAP_X(density->user_mean));
		printf("NEW stdev %g simple %g\n", sqrt(sum[2] / sum[0] - SQR(sum[1] / sum[0])), MAP_STDEV(density->user_stdev, density->user_mean));
	}

	*d_mean = sum[1] / sum[0];
	*d_stdev = sqrt(DMAX(0.0, sum[2] / sum[0] - SQR(*d_mean)));

	/*
	 * adjust the  mode. use a 2.order polynomial
	 *
	 * > ans := solve({ a+b*x0+c*x0^2=d0, a+b*x1+c*x1^2=d1,a+b*x2+c*x2^2=d2}, [a,b,c]):
	 * > a := rhs(ans[1,1]):                                                           
	 * > b := rhs(ans[1,2]):
	 * > c := rhs(ans[1,3]):
	 * > simplify(-b/(2*c));
	 *         2        2        2        2        2        2
	 *    d0 x1  - d0 x2  - d1 x0  + d1 x2  + d2 x0  - d2 x1
	 *    ---------------------------------------------------
	 *      2 (d0 x1 - d0 x2 - d1 x0 + d1 x2 + d2 x0 - d2 x1)
	 */

	double xx[3], tld[3];

	idx_tlogdens_max = IMAX(1, IMIN(npm - 2, idx_tlogdens_max));	/* so that we can do interpolation */
	for (k = 0; k < 3; k++) {
		i = idx_tlogdens_max - 1 + k;
		xx[k] = GMRFLib_density_std2user(xpm[i], density);
		tld[k] = TRANSFORMED_LOGDENS(xx[k], ldm[i]);
		xx[k] = MAP_X(xx[k]);			       /* need it in the transformed scale */
	}
	*d_mode = (tld[0] * xx[1] * xx[1] - tld[0] * xx[2] * xx[2] - tld[1] * xx[0] * xx[0] +
		   tld[1] * xx[2] * xx[2] + tld[2] * xx[0] * xx[0] - tld[2] * xx[1] * xx[1]) / (tld[0] * xx[1] - tld[0] * xx[2] - tld[1] * xx[0] + tld[1] * xx[2] +
												tld[2] * xx[0] - xx[1] * tld[2]) / 0.2e1;

	Free(work);
#undef MAP_X
#undef MAP_STDEV
	return GMRFLib_SUCCESS;
}
int inla_output_detail(const char *dir, GMRFLib_density_tp ** density, GMRFLib_density_tp ** gdensity, double *locations,
		       int n, int nrep, Output_tp * output, const char *sdir,
		       // Either this
		       map_func_tp * func, void *func_arg,
		       // .. or this
		       GMRFLib_transform_array_func_tp ** tfunc,
		       // 
		       const char *tag, const char *modelname, int verbose)
{
#define FUNC (func ? func : NULL)
#define FUNC_ARG (func ? func_arg : NULL)
#define TFUNC(_idx) (tfunc ? tfunc[_idx] : NULL)

#define MAP_DENS(_dens, _x_user, _idx) (func ? (_dens)/(ABS(func(_x_user, MAP_DFORWARD, func_arg))) : \
					(tfunc ? (_dens)/(ABS(tfunc[_idx]->func(_x_user, GMRFLib_TRANSFORM_DFORWARD, tfunc[_idx]->arg, tfunc[_idx]->cov))) : \
					 (_dens)))

#define MAP_X(_x_user, _idx) (func ? func(_x_user, MAP_FORWARD, func_arg) : \
			      (tfunc ? tfunc[_idx]->func(_x_user, GMRFLib_TRANSFORM_FORWARD, tfunc[_idx]->arg, tfunc[_idx]->cov) : \
			       (_x_user)))

#define MAP_INCREASING(_idx) (func ? func(0.0, MAP_INCREASING, func_arg) : \
			      (tfunc ? tfunc[_idx]->func(0.0, GMRFLib_TRANSFORM_INCREASING, tfunc[_idx]->arg, tfunc[_idx]->cov) : 1))

#define MAP_DECREASING(_idx) (!MAP_INCREASING(_idx))

	char *ndir = NULL, *ssdir = NULL, *msg = NULL, *nndir = NULL;
	FILE *fp = NULL;
	double x, x_user, dens, dens_user, p, xp, *xx;
	double d_mean, d_stdev, *d_mode = NULL, *g_mode = NULL;
	int i, ii, j, nn, ndiv;
	int add_empty = 1;

	assert(nrep > 0);
	ndiv = n / nrep;

	d_mode = Calloc(n, double);
	g_mode = Calloc(n, double);

	ssdir = GMRFLib_strdup(sdir);
	GMRFLib_sprintf(&ndir, "%s/%s", dir, ssdir);
	inla_fnmfix(ndir);
	if (inla_mkdir(ndir) != 0) {
		GMRFLib_sprintf(&msg, "fail to create directory [%s]: %s", ndir, strerror(errno));
		inla_error_general(msg);
	}
	Free(ssdir);
	if (1) {
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "N");
		inla_fnmfix(nndir);
		fp = fopen(nndir, "w");
		if (!fp) {
			inla_error_open_file(nndir);
		}
		fprintf(fp, "%d\n", n);
		fclose(fp);
		Free(nndir);
	}
	if (tag) {
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "TAG");
		inla_fnmfix(nndir);
		fp = fopen(nndir, "w");
		if (!fp) {
			inla_error_open_file(nndir);
		}
		fprintf(fp, "%s\n", tag);
		fclose(fp);
		Free(nndir);
	}
	if (modelname) {
		GMRFLib_sprintf(&nndir, "%s/%s", ndir, "MODEL");
		inla_fnmfix(nndir);
		fp = fopen(nndir, "w");
		if (!fp) {
			inla_error_open_file(nndir);
		}
		fprintf(fp, "%s\n", modelname);
		fclose(fp);
		Free(nndir);
	}
	if (output->summary) {
		if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "summary.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore summary results in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (density[i]) {
					inla_integrate_func(&d_mean, &d_stdev, &d_mode[i], density[i], FUNC, FUNC_ARG, TFUNC(i));
					if (locations) {
						if (G.binary) {
							D3W(locations[i % ndiv], d_mean, d_stdev);
						} else {
							fprintf(fp, "%g %.8g %.8g\n", locations[i % ndiv], d_mean, d_stdev);
						}
					} else {
						if (G.binary) {
							ID2W(i, d_mean, d_stdev);
						} else {
							fprintf(fp, "%1d %.8g %.8g\n", i, d_mean, d_stdev);
						}
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								D3W(locations[i % ndiv], NAN, NAN);
							} else {
								fprintf(fp, "%g %.8g %.8g\n", locations[i % ndiv], NAN, NAN);
							}
						} else {
							if (G.binary) {
								ID2W(i, NAN, NAN);
							} else {
								fprintf(fp, "%1d %.8g %.8g\n", i, NAN, NAN);
							}
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
		if (inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "summary-gaussian.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore summary (gaussian) results in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i]) {
					inla_integrate_func(&d_mean, &d_stdev, &g_mode[i], gdensity[i], FUNC, FUNC_ARG, TFUNC(i));
					if (locations) {
						if (G.binary) {
							D3W(locations[i % ndiv], d_mean, d_stdev);
						} else {
							fprintf(fp, "%g %.8g %.8g\n", locations[i % ndiv], d_mean, d_stdev);
						}
					} else {
						if (G.binary) {
							ID2W(i, d_mean, d_stdev);
						} else {
							fprintf(fp, "%1d %.8g %.8g\n", i, d_mean, d_stdev);
						}
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								D3W(locations[i % ndiv], NAN, NAN);
							} else {
								fprintf(fp, "%g %.8g %.8g\n", locations[i % ndiv], NAN, NAN);
							}
						} else {
							if (G.binary) {
								ID2W(i, NAN, NAN);
							} else {
								fprintf(fp, "%1d %.8g %.8g\n", i, NAN, NAN);
							}
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
	}
	if (output->return_marginals || strncmp("hyperparameter", sdir, 13) == 0) {
		if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "marginal-densities.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore marginals in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (density[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					GMRFLib_density_layout_x(&xx, &nn, density[i]);
					if (G.binary) {
						IW(nn);
					}
					for (ii = 0; ii < nn; ii++) {
						x = xx[ii];
						x_user = GMRFLib_density_std2user(x, density[i]);
						GMRFLib_evaluate_density(&dens, x, density[i]);
						dens_user = dens / density[i]->std_stdev;
						if (G.binary) {
							D2W(MAP_X(x_user, i), MAP_DENS(dens_user, x_user, i));
						} else {
							fprintf(fp, " %.6g %.6g", MAP_X(x_user, i), MAP_DENS(dens_user, x_user, i));
						}
					}
					Free(xx);
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						nn = 3;
						if (G.binary) {
							IW(nn);
						}
						for (ii = 0; ii < nn; ii++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %.6g %.6g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
		if (inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "marginal-densities-gaussian.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore marginal-densities (gaussian) in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						fprintf(fp, "%1d ", i);
					}
					GMRFLib_density_layout_x(&xx, &nn, gdensity[i]);
					if (G.binary) {
						IW(nn);
					}
					for (ii = 0; ii < nn; ii++) {
						x = xx[ii];
						x_user = GMRFLib_density_std2user(x, gdensity[i]);
						GMRFLib_evaluate_density(&dens, x, gdensity[i]);
						dens_user = dens / gdensity[i]->std_stdev;
						if (G.binary) {
							D2W(MAP_X(x_user, i), MAP_DENS(dens_user, x_user, i));
						} else {
							fprintf(fp, " %.6g %.6g", MAP_X(x_user, i), MAP_DENS(dens_user, x_user, i));
						}
					}
					Free(xx);
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						nn = 3;
						if (G.binary) {
							IW(nn);
						}
						for (ii = 0; ii < nn; ii++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %.6g %.6g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
	}
	if (output->kld) {
		/*
		 * this is ok for FUNC as well, since the the KL is invariant for parameter transformations. 
		 */
		if (inla_computed(density, n) && inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "symmetric-kld.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore (symmetric) kld's in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i] && density[i]) {
					double kld;

					if (G.fast_mode) {
						GMRFLib_mkld_sym(&kld, gdensity[i], density[i]);
					} else {
						GMRFLib_kld_sym(&kld, gdensity[i], density[i]);
					}
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						DW(kld);
					} else {
						fprintf(fp, "%.6g\n", kld);
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							DW(NAN);
						} else {
							fprintf(fp, "%.6g\n", NAN);
						}
					}
				}
			}
			fclose(fp);
		} else if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "symmetric-kld.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore (symmetric) kld's in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				GMRFLib_density_tp *gd = NULL;
				if (density[i]) {
					double kld;
					GMRFLib_density_create_normal(&gd, 0.0, 1.0, density[i]->std_mean, density[i]->std_stdev);

					if (G.fast_mode) {
						GMRFLib_mkld_sym(&kld, gd, density[i]);
					} else {
						GMRFLib_kld_sym(&kld, gd, density[i]);
					}
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						DW(kld);
					} else {
						fprintf(fp, "%.6g\n", kld);
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							DW(NAN);
						} else {
							fprintf(fp, "%.6g\n", NAN);
						}
					}
				}
				GMRFLib_free_density(gd);
			}
			fclose(fp);
		}
	}
	if (output->nquantiles) {
		if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "quantiles.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore quantiles in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (density[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						IW(output->nquantiles);
					}
					for (j = 0; j < output->nquantiles; j++) {
						p = output->quantiles[j];
						if (MAP_INCREASING(i)) {
							GMRFLib_density_Pinv(&xp, p, density[i]);
						} else {
							GMRFLib_density_Pinv(&xp, 1.0 - p, density[i]);
						}
						x_user = GMRFLib_density_std2user(xp, density[i]);
						if (G.binary) {
							D2W(p, MAP_X(x_user, i));
						} else {
							fprintf(fp, " %g %g", p, MAP_X(x_user, i));
						}
					}
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							IW(output->nquantiles);
						}
						for (j = 0; j < output->nquantiles; j++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %g %g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
		if (inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "quantiles-gaussian.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore quantiles (gaussian) in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						IW(output->nquantiles);
					}
					for (j = 0; j < output->nquantiles; j++) {
						p = output->quantiles[j];
						if (MAP_INCREASING(i)) {
							GMRFLib_density_Pinv(&xp, p, gdensity[i]);
						} else {
							GMRFLib_density_Pinv(&xp, 1.0 - p, gdensity[i]);
						}
						x_user = GMRFLib_density_std2user(xp, gdensity[i]);
						if (G.binary) {
							D2W(p, MAP_X(x_user, i));
						} else {
							fprintf(fp, " %g %g", p, MAP_X(x_user, i));
						}
					}
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							IW(output->nquantiles);
						}
						for (j = 0; j < output->nquantiles; j++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %g %g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
	}
	if (output->mode) {
		if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "mode.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore mode in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (density[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						D3W(1.0, NAN, d_mode[i]);
					} else {
						fprintf(fp, " %g\n", d_mode[i]);
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							D3W(1.0, NAN, NAN);
						} else {
							fprintf(fp, " %g\n", NAN);
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
		if (inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "mode-gaussian.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore mode in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						D2W(1.0, g_mode[i]);
					} else {
						fprintf(fp, " %g\n", g_mode[i]);
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							D2W(1.0, NAN);
						} else {
							fprintf(fp, " %g\n", NAN);
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
	}
	if (output->ncdf) {
		if (inla_computed(density, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "cdf.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore cdf in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (density[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						IW(output->ncdf);
					}
					for (j = 0; j < output->ncdf; j++) {
						xp = output->cdf[j];
						x = GMRFLib_density_user2std(xp, density[i]);
						GMRFLib_density_P(&p, x, density[i]);
						if (MAP_DECREASING(i)) {
							p = 1.0 - p;
						}
						if (G.binary) {
							D2W(MAP_X(xp, i), p);
						} else {
							fprintf(fp, " %g %g", MAP_X(xp, i), p);
						}
					}
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							IW(output->ncdf);
						}
						for (j = 0; j < output->ncdf; j++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %g %g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
		if (inla_computed(gdensity, n)) {
			GMRFLib_sprintf(&nndir, "%s/%s", ndir, "cdf-gaussian.dat");
			inla_fnmfix(nndir);
			fp = fopen(nndir, (G.binary ? "wb" : "w"));
			if (!fp) {
				inla_error_open_file(nndir);
			}
			if (verbose) {
#pragma omp critical
				{
					printf("\t\tstore cdf (gaussian) in[%s]\n", nndir);
				}
			}
			for (i = 0; i < n; i++) {
				if (gdensity[i]) {
					if (locations) {
						if (G.binary) {
							DW(locations[i % ndiv]);
						} else {
							fprintf(fp, "%g ", locations[i % ndiv]);
						}
					} else {
						if (G.binary) {
							IW(i);
						} else {
							fprintf(fp, "%1d ", i);
						}
					}
					if (G.binary) {
						IW(output->ncdf);
					}
					for (j = 0; j < output->ncdf; j++) {
						xp = output->cdf[j];
						x = GMRFLib_density_user2std(xp, gdensity[i]);
						GMRFLib_density_P(&p, MAP_X(x, i), gdensity[i]);
						if (MAP_DECREASING(i)) {
							p = 1.0 - p;
						}
						if (G.binary) {
							D2W(xp, p);
						} else {
							fprintf(fp, " %g %g", MAP_X(xp, i), p);
						}
					}
					if (!G.binary) {
						fprintf(fp, "\n");
					}
				} else {
					if (add_empty) {
						if (locations) {
							if (G.binary) {
								DW(locations[i % ndiv]);
							} else {
								fprintf(fp, "%g ", locations[i % ndiv]);
							}
						} else {
							if (G.binary) {
								IW(i);
							} else {
								fprintf(fp, "%1d ", i);
							}
						}
						if (G.binary) {
							IW(output->ncdf);
						}
						for (j = 0; j < output->ncdf; j++) {
							if (G.binary) {
								D2W(NAN, NAN);
							} else {
								fprintf(fp, " %g %g", NAN, NAN);
							}
						}
						if (!G.binary) {
							fprintf(fp, "\n");
						}
					}
				}
			}
			fclose(fp);
			Free(nndir);
		}
	}

	Free(d_mode);
	Free(g_mode);

#undef MAP_DENS
#undef MAP_X
#undef MAP_INCREASING
#undef MAP_DECREASING
#undef FUNC
#undef FUNC_ARG
	return INLA_OK;
}
int my_setenv(char *str, int prefix)
{
	/*
	 * set a variable in the enviroment; if PREFIX prepend with inla_, so that a=b yields inla_a=b. 
	 */
	char *p = NULL, *var = NULL;
	int debug = 0;

	if (debug)
		printf("enter my_setenv with [%s]\n", str);

	p = strchr(str, '=');
	if (!p) {
		fprintf(stderr, "*** Error: Argument is void [%s]\n", str);
		exit(EXIT_FAILURE);
	}
	*p = '\0';
#if defined(WINDOWS)
	if (prefix) {
		GMRFLib_sprintf(&var, "inla_%s=%s", str, p + 1);
	} else {
		GMRFLib_sprintf(&var, "%s=%s", str, p + 1);
	}
	putenv(var);
	if (debug)
		printf("putenv \t%s\n", var);
#else
	if (prefix) {
		GMRFLib_sprintf(&var, "inla_%s", str);
	} else {
		GMRFLib_sprintf(&var, "%s", str);
	}
	setenv(var, p + 1, 1);
	if (debug)
		printf("\tsetenv %s=%s\n", var, p + 1);
#endif
	Free(var);
	return INLA_OK;
}
void inla_signal(int sig)
{
#if !defined(WINDOWS)
	switch (sig) {
	case SIGUSR1:
		GMRFLib_timer_full_report(NULL);
		break;

	case SIGUSR2:
		fprintf(stderr, "\n\n\t%s: Recieve signal %1d: request optimiser to stop\n\n", __GMRFLib_FuncName, sig);
		GMRFLib_request_optimiser_to_stop = GMRFLib_TRUE;
		break;

	default:
		break;
	}
#endif
	return;
}
int inla_endian(void)
{
	int x = 1;
	return ((*(char *) &x) ? INLA_LITTLE_ENDIAN : INLA_BIG_ENDIAN);
}
int inla_divisible(int n, int by)
{
	/*
	 * same function as inla.divisi 
	 */

	if (by == 0)
		return GMRFLib_TRUE;

	if (by > 0)
		return (by * (n / by) == n ? GMRFLib_TRUE : GMRFLib_FALSE);
	else
		return ((-by) * (n / (-by)) == n ? GMRFLib_FALSE : GMRFLib_TRUE);
}
int inla_qinv(const char *filename, const char *constrfile, const char *outfile)
{
	/*
	 * Compute the marginal variances for Cij file in FILENAME and output on stdout, the marginal variances 
	 */
	int i, j, jj, k;

	GMRFLib_tabulate_Qfunc_tp *tab;
	GMRFLib_graph_tp *graph;
	GMRFLib_problem_tp *problem;
	GMRFLib_constr_tp *constr = NULL;
	GMRFLib_matrix_tp *constr_x = NULL;
	FILE *fp;

	GMRFLib_tabulate_Qfunc_from_file(&tab, &graph, filename, -1, NULL, NULL, NULL);
	fp = fopen(constrfile, "r");
	if (fp) {
		fclose(fp);
		constr_x = GMRFLib_read_fmesher_file(constrfile, 0L, SEEK_CUR);
		if (constr_x->A[0] > 0) {
			constr = Calloc(1, GMRFLib_constr_tp);
			constr->nc = (int) constr_x->A[0];
			constr->a_matrix = &constr_x->A[1];
			constr->e_vector = &constr_x->A[constr->nc * graph->n + 1];
			GMRFLib_prepare_constr(constr, graph, 1);
		}
	}

	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(graph, NULL, NULL, NULL);
	}
	GMRFLib_init_problem(&problem, NULL, NULL, NULL, NULL, graph, tab->Qfunc, tab->Qfunc_arg, NULL, constr, GMRFLib_NEW_PROBLEM);
	GMRFLib_Qinv(problem, GMRFLib_QINV_ALL);

	/*
	 * write a fmesher file and just pass the filename 
	 */
	GMRFLib_matrix_tp *M = Calloc(1, GMRFLib_matrix_tp);

	M->nrow = graph->n;
	M->ncol = graph->n;
	M->elems = 0;
	for (i = 0; i < graph->n; i++) {
		M->elems += 1 + graph->nnbs[i];
	}

	M->i = Calloc(M->elems, int);
	M->j = Calloc(M->elems, int);
	M->values = Calloc(M->elems, double);

	k = 0;
	for (i = 0; i < graph->n; i++) {
		M->i[k] = i;
		M->j[k] = i;
		M->values[k] = *GMRFLib_Qinv_get(problem, i, i);
		k++;

		for (jj = 0; jj < graph->nnbs[i]; jj++) {
			j = graph->nbs[i][jj];
			M->i[k] = i;
			M->j[k] = j;
			M->values[k] = *GMRFLib_Qinv_get(problem, i, j);
			k++;
		}
	}
	assert(k == M->elems);

	GMRFLib_write_fmesher_file(M, outfile, (long int) 0, -1);

	return 0;
}
int inla_qsolve(const char *Qfilename, const char *Afilename, const char *Bfilename, const char *method)
{
	/*
	 * Solve Q X = B, L^T X = B, or L X = B
	 */

	GMRFLib_tabulate_Qfunc_tp *tab;
	GMRFLib_graph_tp *graph;
	GMRFLib_problem_tp *problem;
	int i;

	GMRFLib_tabulate_Qfunc_from_file(&tab, &graph, Qfilename, -1, NULL, NULL, NULL);
	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(graph, NULL, NULL, NULL);
	}
	GMRFLib_init_problem(&problem, NULL, NULL, NULL, NULL, graph, tab->Qfunc, tab->Qfunc_arg, NULL, NULL, GMRFLib_NEW_PROBLEM);

	/*
	 * I need B to be dense 
	 */
	GMRFLib_matrix_tp *B = GMRFLib_read_fmesher_file(Bfilename, (long int) 0, -1);
	assert(B->i == NULL);				       /* I want B as dense matrix */
	assert(problem->n == B->nrow);

#pragma omp parallel for private(i)
	for (i = 0; i < B->ncol; i++) {
		if (!strcasecmp(method, "solve")) {
			GMRFLib_solve_llt_sparse_matrix(&(B->A[i * B->nrow]), &(problem->sub_sm_fact), problem->sub_graph);
		} else if (!strcasecmp(method, "forward")) {
			GMRFLib_solve_l_sparse_matrix(&(B->A[i * B->nrow]), &(problem->sub_sm_fact), problem->sub_graph);
		} else if (!strcasecmp(method, "backward")) {
			GMRFLib_solve_lt_sparse_matrix(&(B->A[i * B->nrow]), &(problem->sub_sm_fact), problem->sub_graph);
		} else {
			assert(0 == 1);
		}
	}

	B->iA = NULL;
	GMRFLib_write_fmesher_file(B, Afilename, (long int) 0, -1);

	return 0;
}

int inla_qsample(const char *filename, const char *outfile, const char *nsamples, const char *rngfile,
		 const char *samplefile, const char *bfile, const char *mufile, const char *constrfile)
{
	size_t siz, ret;
	char *state;
	FILE *fp;

	fp = fopen(rngfile, "rb");
	if (fp) {
		fseek(fp, 0L, SEEK_END);
		siz = ftell(fp) + 1;
		rewind(fp);
		state = Calloc(siz, char);
		ret = fread((void *) state, (size_t) 1, siz, fp);
		if (ret > 0) {
			GMRFLib_uniform_setstate((void *) state);
		}
		fclose(fp);
		Free(state);
	}

	int i, ns = atoi(nsamples);
	GMRFLib_tabulate_Qfunc_tp *tab;
	GMRFLib_graph_tp *graph;
	GMRFLib_problem_tp *problem;

	GMRFLib_matrix_tp *M = Calloc(1, GMRFLib_matrix_tp), *S = NULL, *b = NULL, *mu = NULL, *constr_x = NULL;
	GMRFLib_constr_tp *constr = NULL;

	GMRFLib_tabulate_Qfunc_from_file(&tab, &graph, filename, -1, NULL, NULL, NULL);

	fp = fopen(samplefile, "r");
	if (fp) {
		fclose(fp);				       /* file exists */
		S = GMRFLib_read_fmesher_file(samplefile, 0L, SEEK_CUR);
	}

	fp = fopen(bfile, "r");
	if (fp) {
		fclose(fp);				       /* file exists */
		b = GMRFLib_read_fmesher_file(bfile, 0L, SEEK_CUR);
	}

	fp = fopen(mufile, "r");
	if (fp) {
		fclose(fp);				       /* file exists */
		mu = GMRFLib_read_fmesher_file(mufile, 0L, SEEK_CUR);
	}

	fp = fopen(constrfile, "r");
	if (fp) {
		fclose(fp);
		constr_x = GMRFLib_read_fmesher_file(constrfile, 0L, SEEK_CUR);
		if (constr_x->A[0] > 0) {
			constr = Calloc(1, GMRFLib_constr_tp);
			constr->nc = (int) constr_x->A[0];
			constr->a_matrix = &constr_x->A[1];
			constr->e_vector = &constr_x->A[constr->nc * graph->n + 1];
			GMRFLib_prepare_constr(constr, graph, 1);
		}
	}

	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(graph, NULL, NULL, NULL);
	}
	GMRFLib_init_problem(&problem, NULL, (b ? b->A : NULL), NULL, (mu ? mu->A : NULL), graph, tab->Qfunc, tab->Qfunc_arg, NULL, constr, GMRFLib_NEW_PROBLEM);

	M->nrow = graph->n + 1;
	M->ncol = ns;
	M->elems = M->ncol * M->nrow;
	M->A = Calloc(M->nrow * M->ncol, double);

	for (i = 0; i < ns; i++) {
		if (!S) {
			GMRFLib_sample(problem);
		} else {
			memcpy(problem->sample, &(S->A[i * S->nrow]), S->nrow * sizeof(double));
		}
		GMRFLib_evaluate(problem);
		memcpy(&(M->A[i * M->nrow]), problem->sample, M->nrow * sizeof(double));
		M->A[(i + 1) * M->nrow - 1] = problem->sub_logdens;
	}

	GMRFLib_write_fmesher_file(M, outfile, (long int) 0, -1);

	state = GMRFLib_rng_getstate(&siz);
	fp = fopen(rngfile, "wb");
	fwrite((void *) state, (size_t) 1, siz, fp);
	fclose(fp);

	return 0;
}

int inla_finn(const char *filename)
{
	/*
	 * Compute whatever Finn wants...
	 */
	int i;
	GMRFLib_tabulate_Qfunc_tp *tab;
	GMRFLib_graph_tp *graph;
	GMRFLib_problem_tp *problem;

	GMRFLib_tabulate_Qfunc_from_file(&tab, &graph, filename, -1, NULL, NULL, NULL);

	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(graph, NULL, NULL, NULL);
	}
	GMRFLib_init_problem(&problem, NULL, NULL, NULL, NULL, graph, tab->Qfunc, tab->Qfunc_arg, NULL, NULL, GMRFLib_NEW_PROBLEM);
	GMRFLib_sample(problem);

	for (i = 0; i < graph->n; i++) {
		printf("%.20g\n", problem->sample[i]);
	}
	for (i = 0; i < graph->n; i++) {
		printf("%1d\n", problem->sub_sm_fact.remap[i]);
	}

	return 0;
}

int inla_qreordering(const char *filename)
{
	/*
	 * return the rordering either given or computed
	 */
	int i;
	GMRFLib_graph_tp *graph;

	if (GMRFLib_is_fmesher_file(filename, (long int) 0, -1) == GMRFLib_SUCCESS) {
		GMRFLib_tabulate_Qfunc_tp *qtab = NULL;
		GMRFLib_tabulate_Qfunc_from_file(&qtab, &graph, filename, -1, NULL, NULL, NULL);
		GMRFLib_free_tabulate_Qfunc(qtab);
	} else {
		GMRFLib_read_graph(&graph, filename);
	}

	if (G.reorder < 0) {
		GMRFLib_optimize_reorder(graph, NULL, NULL, NULL);
	}
	GMRFLib_sm_fact_tp sm_fact;
	sm_fact.smtp = GMRFLib_SMTP_TAUCS;
	GMRFLib_compute_reordering(&sm_fact, graph, NULL);

	printf("%s\n", GMRFLib_reorder_name(GMRFLib_reorder));
	printf("%1d\n", GMRFLib_reorder);
	for (i = 0; i < graph->n; i++) {
		printf("%1d\n", sm_fact.remap[i]);
	}

	return 0;
}

int inla_read_graph(const char *filename)
{
	/*
	 * Read a graph and print it on stdio. Compute also the connected components.
	 */
	GMRFLib_graph_tp *graph = NULL;

	GMRFLib_read_graph(&graph, filename);
	GMRFLib_write_graph_2(stdout, graph);

	int *cc, i;
	cc = GMRFLib_connected_components(graph);
	for (i = 0; i < graph->n; i++)
		printf("%d\n", cc[i]);
	Free(cc);

	return 0;
}

inla_file_contents_tp *inla_read_file_contents(const char *filename)
{
	/*
	 * just read the hole file into on long character vector 
	 */

	FILE *fp;
	long len;

	if (!filename) {
		return NULL;
	}
	fp = fopen(filename, "rb");
	if (!fp) {
		return NULL;
	}
	fseek(fp, 0L, SEEK_END);
	len = ftell(fp);
	assert(len > 0L);

	inla_file_contents_tp *fc = Calloc(1, inla_file_contents_tp);
	fc->contents = Calloc((size_t) len, char);

	rewind(fp);
	fc->len = fread(fc->contents, (size_t) 1, (size_t) len, fp);
	assert(fc->len == (size_t) len);
	fclose(fp);

	return fc;
}
int inla_write_file_contents(const char *filename, inla_file_contents_tp * fc)
{
	/*
	 * just dump the file contents to the new file 
	 */

	if (!fc) {
		return INLA_OK;
	}

	FILE *fp;
	size_t len;

	fp = fopen(filename, "wb");
	assert(fp);
	len = fwrite(fc->contents, (size_t) 1, fc->len, fp);
	assert(len == fc->len);

	fclose(fp);
	return INLA_OK;
}

int inla_besag_scale_OLD(inla_besag_Qfunc_arg_tp * arg)
{
	/*
	 * OLD version: SLOW
	 */

	inla_besag_Qfunc_arg_tp *def = Calloc(1, inla_besag_Qfunc_arg_tp);
	inla_besag_Qfunc_arg_tp *odef = arg;

	GMRFLib_copy_graph(&(def->graph), odef->graph);
	def->log_prec = NULL;
	def->prec_scale = NULL;

	gsl_matrix *Q = gsl_matrix_alloc(def->graph->n, def->graph->n);
	size_t i, j, k;

	/*
	 * yes, use dense matrix
	 */
	for (i = 0; i < (size_t) def->graph->n; i++) {
		gsl_matrix_set(Q, i, i, Qfunc_besag(i, i, def));
		for (k = 0; k < (size_t) def->graph->nnbs[i]; k++) {
			double value;

			j = def->graph->nbs[i][k];
			value = Qfunc_besag(i, j, def);
			gsl_matrix_set(Q, i, j, value);
			gsl_matrix_set(Q, j, i, value);
		}
	}

	FIXME("ASSUME NCC=1!!!!");
	GMRFLib_gsl_ginv(Q, -1, 1);
	double sum = 0.0, scale;

	for (i = 0; i < (size_t) def->graph->n; i++) {
		sum += log(gsl_matrix_get(Q, i, i));
	}
	scale = exp(sum / def->graph->n);

	odef->prec_scale = Calloc(1, double);
	odef->prec_scale[0] = scale;

	gsl_matrix_free(Q);
	GMRFLib_free_graph(def->graph);
	Free(def);

	return GMRFLib_SUCCESS;
}
int inla_besag_scale(inla_besag_Qfunc_arg_tp * arg, int adj)
{
	inla_besag_Qfunc_arg_tp *def = Calloc(1, inla_besag_Qfunc_arg_tp);
	inla_besag_Qfunc_arg_tp *odef = arg;

	GMRFLib_copy_graph(&(def->graph), odef->graph);
	def->log_prec = NULL;
	def->prec_scale = NULL;

	int i, j, *cc = NULL, n = def->graph->n;
	if (adj) {
		cc = GMRFLib_connected_components(def->graph);
	} else {
		cc = Calloc(n, int);
	}

	double *c = Calloc(def->graph->n, double), eps = GMRFLib_eps(0.75);
	for (i = 0; i < n; i++) {
		c[i] = eps;
	}

	GMRFLib_constr_tp *constr = NULL;
	GMRFLib_make_empty_constr(&constr);

	int ncc = 1 + GMRFLib_imax_value(cc, def->graph->n, NULL);	/* number of the connected components */
	int *c_cc = Calloc(ncc, int);			       /* the number of nodes in each group */

	// check that we have at least two in each group
	for (i = 0; i < n; i++) {
		c_cc[cc[i]]++;
	}
	for (j = 0; j < ncc; j++) {
		assert(c_cc[j] > 0);			       /* this should not happend */
		if (c_cc[j] == 1) {
			char *msg;
			GMRFLib_sprintf(&msg,
					"One of the %1d connected components in the graph for the Besag model, has only 1 element; fail to apply scale.model=TRUE",
					ncc);
			inla_error_general(msg);
			exit(1);
		}
	}
	Free(c_cc);

	constr->nc = ncc;
	constr->a_matrix = Calloc(n * constr->nc, double);
	for (i = 0; i < n; i++) {
		j = cc[i];
		constr->a_matrix[i * constr->nc + j] = 1.0;
	}
	constr->e_vector = Calloc(constr->nc, double);
	GMRFLib_prepare_constr(constr, def->graph, GMRFLib_TRUE);
	// GMRFLib_print_constr(stdout, constr, def->graph);

	GMRFLib_problem_tp *problem;

	int retval = GMRFLib_SUCCESS, ok = 0, num_try = 0, num_try_max = 100;
	GMRFLib_error_handler_tp *old_handler = GMRFLib_set_error_handler_off();

	while (!ok) {
		retval = GMRFLib_init_problem(&problem, NULL, NULL, c, NULL, def->graph, Qfunc_besag, (void *) def, NULL, constr, GMRFLib_NEW_PROBLEM);
		switch (retval) {
		case GMRFLib_EPOSDEF:
		{
			for (i = 0; i < n; i++) {
				c[i] *= 10.0;
			}
			problem = NULL;
			break;
		}
		case GMRFLib_SUCCESS:
			ok = 1;
			break;
		default:
			GMRFLib_set_error_handler(old_handler);
			GMRFLib_ERROR(retval);
			abort();
			break;
		}
		if (++num_try >= num_try_max) {
			FIXME("This should not happen. Contact developers...");
			abort();
		}
	}
	GMRFLib_set_error_handler(old_handler);

	GMRFLib_Qinv(problem, GMRFLib_QINV_DIAG);

	double sum = 0.0;
	for (i = 0; i < n; i++) {
		sum += log(*(GMRFLib_Qinv_get(problem, i, i)));
	}

	arg->prec_scale = Calloc(1, double);
	arg->prec_scale[0] = exp(sum / def->graph->n);

	GMRFLib_free_problem(problem);
	GMRFLib_free_graph(def->graph);
	GMRFLib_free_constr(constr);
	Free(def);
	Free(cc);

	return GMRFLib_SUCCESS;
}
double inla_update_density(double *theta, inla_update_tp * arg)
{
	/*
	 * joint posterior for theta
	 */

	int i, corr = (arg->stdev_corr_pos && arg->stdev_corr_neg);
	double value = 0.0, sd, log_nc, update_dens, *z;

	z = Calloc(arg->ntheta, double);
	GMRFLib_ai_theta2z(z, arg->ntheta, arg->theta_mode, theta, arg->sqrt_eigen_values, arg->eigen_vectors);

	for (i = 0; i < arg->ntheta; i++) {
		if (corr) {
			sd = (z[i] > 0 ? arg->stdev_corr_pos[i] : arg->stdev_corr_neg[i]);
		} else {
			sd = 1.0;
		}
		value += -0.5 * SQR(z[i] / sd);
	}

	/*
	 * this is the normalizing constant
	 */
	log_nc = 0.5 * arg->ntheta * log(2.0 * M_PI);
	for (i = 0; i < arg->ntheta; i++) {
		if (corr) {
			log_nc += -0.5 * (2.0 * log(gsl_vector_get(arg->sqrt_eigen_values, (unsigned int) i)) +
					  0.5 * (log(SQR(arg->stdev_corr_pos[i])) + log(SQR(arg->stdev_corr_neg[i]))));
		} else {
			log_nc += -0.5 * (2.0 * log(gsl_vector_get(arg->sqrt_eigen_values, (unsigned int) i)));
		}
	}

	/*
	 * and then the log-joint-posterior-density
	 */
	update_dens = value - log_nc;

	Free(z);
	return update_dens;
}
int testit(int argc, char **argv)
{
	if (1) {
		double x;
		for (x = -100.0; x < 100.0; x = x + 0.01) {
			printf("x %.12g log(Phi(x)) %.12g %.12g\n", x, inla_log_Phi_fast(x), inla_log_Phi(x));
		}
		exit(0);
	}
	if (1) {
		// checking the expression and the jacobian for this prior
		double x, xx, dx = 0.001, sum = 0.0, parameters[2];

		parameters[0] = 3.123;			       /* lambda */
		parameters[1] = 2;			       /* p */
		for (x = -6.001; x < 6.0; x += dx) {
			for (xx = -6.001; xx < 6.0; xx += dx) {
				double x2[2];
				x2[0] = x;
				x2[1] = xx;
				sum += exp(priorfunc_pc_ar(x2, parameters)) * SQR(dx);
			}
		}
		P(sum);
	}

	if (0) {
		// test the new R-interface

		printf("TESTIT!\n");
		inla_R_source("example-code.R");
		double x[] = { 1, 2, 3 };
		int nx = sizeof(x) / sizeof(x[1]);

		double *xx = NULL;
		int nxx;
		int i;
#pragma omp parallel for private(i)
		for (i = 0; i < 10; i++) {
			inla_R_funcall2(&nxx, &xx, "lprior2", "ThisIsTheTag", nx, x);
		}
		inla_R_funcall2(&nxx, &xx, "lprior2", NULL, nx, x);

		for (i = 0; i < nxx; i++) {
			printf("lprior2[%1d] = %f\n", i, xx[i]);
		}

		exit(0);
	}

	if (0) {
		// testing spde3

		inla_spde3_tp *smodel = NULL;
		int i, j, jj, n, t, p;

		inla_spde3_build_model(&smodel, "./", "identity");
		n = smodel->graph->n;
		p = smodel->ntheta;

		GMRFLib_matrix_tp *theta = GMRFLib_read_fmesher_file("theta", 0, -1);

		for (i = 0; i < p; i++) {
			printf("theta[%1d] = %g\n", i, GMRFLib_matrix_get(i, 0, theta));
			for (t = 0; t < GMRFLib_MAX_THREADS; t++) {
				smodel->theta[i][t][0] = GMRFLib_matrix_get(i, 0, theta);
			}
		}

		GMRFLib_matrix_tp *Q = Calloc(1, GMRFLib_matrix_tp);
		Q->nrow = Q->ncol = n;
		Q->elems = ISQR(n);
		Q->A = Calloc(ISQR(n), double);


		int ntimes = 10, itim;

		for (itim = 0; itim < ntimes; itim++) {
#pragma omp parallel for private(i, jj, j)
			for (i = 0; i < n; i++) {
				Q->A[i + i * n] = inla_spde3_Qfunction(i, i, (void *) smodel);
				for (jj = 0; jj < smodel->graph->nnbs[i]; jj++) {
					j = smodel->graph->nbs[i][jj];
					Q->A[i + j * n] = Q->A[j + i * n] = inla_spde3_Qfunction(i, j, (void *) smodel);
				}
			}
		}
		GMRFLib_write_fmesher_file(Q, "Q", 0, -1);
	}

	if (0) {
		GMRFLib_verify_graph_read_from_disc = GMRFLib_TRUE;
		GMRFLib_graph_tp *graph;
		GMRFLib_read_graph_ascii(&graph, "zones.graph");
		exit(0);
	}

	if (0) {
		double a_data[] = { 0.18, 0.41, 0.14, 0.51,
			0.60, 0.24, 0.30, 0.13,
			0.57, 0.99, 0.97, 0.19,
			0.96, 0.58, 0.66, 0.85
		};

		gsl_matrix_view m = gsl_matrix_view_array(a_data, 4, 4);
		gsl_matrix *A = GMRFLib_gsl_duplicate_matrix(&m.matrix);

		GMRFLib_gsl_matrix_fprintf(stdout, A, " %.12f");
		printf("\n");
		GMRFLib_gsl_ginv(A, GMRFLib_eps(0.5), -1);
		GMRFLib_gsl_matrix_fprintf(stdout, A, " %.12f");

		exit(EXIT_SUCCESS);
	}

	if (0) {
		double lambda = 10;
		re_init(&lambda);
	}
	if (0) {
		ar_test1();
		exit(EXIT_SUCCESS);
	}

	if (0) {
		P(bessel_Knu(0.25, 0.2));
		P(gsl_sf_bessel_Knu(0.25, 0.2));
		exit(EXIT_SUCCESS);
	}


	if (0) {
		P(re_intrinsic_discrepancy_distance_map(re_intrinsic_discrepancy_distance(0.1, 3.2)));
		P(re_intrinsic_discrepancy_distance_map(re_intrinsic_discrepancy_distance(0.5, 3.2)));
		exit(EXIT_SUCCESS);
	}


	if (0) {
#define GET(_int) fscanf(fp, "%d\n", &_int)
#define GETV(_vec, _len)						\
		if (1) {						\
			_vec = Calloc(_len, double);			\
			int _i;						\
			for(_i=0; _i < _len; _i++) {			\
				fscanf(fp, "%lf\n", &_vec[_i]);		\
				if (0) printf("%s[%1d] = %g\n", #_vec, _i, _vec[_i]); \
			}						\
		}

		if (1) {

			FILE *fp = fopen("prior.dat", "r");

			int nx, ny, nz;
			double *x, *y, *z;

			GET(nx);
			GET(ny);
			GET(nz);
			GETV(x, nx);
			GETV(y, ny);
			GETV(z, nz);

			int i;
			double lev = 0.1;

			for (i = 0; i < nz; i++) {
				if (z[i] < 0.0) {
					z[i] = NAN;
				}
			}

			re_contour_tp *c;
			c = contourLines(x, nx, y, ny, z, lev);

			re_print_contourLines(NULL, c);
		}
	}

	if (0) {
		inla_file_contents_tp *fc;

		fc = inla_read_file_contents("aa.dat");
		inla_write_file_contents("bb.dat", fc);
		exit(EXIT_SUCCESS);
	}

	if (0) {
		GMRFLib_matrix_tp *M = NULL;

		int i, j, k, kk;


		printf("read file %s\n", argv[3]);
		M = GMRFLib_read_fmesher_file(argv[3], 0L, -1);

		if (1)
			if (M->i)
				for (k = 0; k < M->elems; k++)
					printf("k %d %d %d %g\n", k, M->i[k], M->j[k], M->values[k]);

		if (M->graph) {
			printf("n %d\n", M->graph->n);
			for (k = 0; k < M->graph->n; k++) {
				printf("%d nnbs %d:\n", k, M->graph->nnbs[k]);
				for (kk = 0; kk < M->graph->nnbs[k]; kk++)
					printf("\t\t%d\n", M->graph->nbs[k][kk]);
			}
		}

		for (i = 0; i < M->nrow; i++)
			for (j = 0; j < M->ncol; j++)
				printf("%d %d %g\n", i, j, GMRFLib_matrix_get(i, j, M));

		printf("\n\ntranspose...\n\n\n");
		GMRFLib_matrix_tp *N = GMRFLib_matrix_transpose(M);

		if (1)
			if (N->i)
				for (k = 0; k < N->elems; k++)
					printf("k %d %d %d %g\n", k, N->i[k], N->j[k], N->values[k]);

		if (1)
			for (i = 0; i < N->nrow; i++)
				for (j = 0; j < N->ncol; j++)
					printf("%d %d %g\n", i, j, GMRFLib_matrix_get(i, j, N));

		if (N->graph) {
			printf("n %d\n", N->graph->n);
			for (k = 0; k < N->graph->n; k++) {
				printf("%d nnbs %d:\n", k, N->graph->nnbs[k]);
				for (kk = 0; kk < N->graph->nnbs[k]; kk++)
					printf("\t\t%d\n", N->graph->nbs[k][kk]);
			}
		}
		GMRFLib_matrix_free(M);
		GMRFLib_matrix_free(N);
	}

	exit(EXIT_SUCCESS);
}
int main(int argc, char **argv)
{
#define USAGE_intern(fp)  fprintf(fp, "\nUsage: %s [-v] [-V] [-h] [-f] [-e var=value] [-t MAX_THREADS] [-m MODE] FILE.INI\n", program)
#define USAGE USAGE_intern(stderr)
#define HELP USAGE_intern(stdout);					\
	printf("\t\t-v\t: Verbose output.\n");				\
	printf("\t\t-V\t: Print version and exit.\n");			\
	printf("\t\t-b\t: Use binary output-files.\n");			\
	printf("\t\t-s\t: Be silent.\n");				\
	printf("\t\t-c\t: Create core-file if needed (and allowed). (Linux/MacOSX only.)\n"); \
	printf("\t\t-R\t: Restart using previous mode.\n");		\
	printf("\t\t-e var=value\t: Set variable VAR to VALUE.\n");	\
	printf("\t\t-t MAX_THREADS\t: set the maximum number of threads.\n"); \
	printf("\t\t-m MODE\t: Enable special mode:\n");		\
        printf("\t\t\tMCMC  :  Enable MCMC mode\n");			\
        printf("\t\t\tHYPER :  Enable HYPERPARAMETER mode\n");		\
	printf("\t\t-h\t: Print (this) help.\n")

#define BUGS_intern(fp) fprintf(fp, "Report bugs to <help@r-inla.org>\n")
#define BUGS BUGS_intern(stdout)
	int i, verbose = 0, silent = 0, opt, report = 0, arg, nt, err, ncpu, enable_core_file = 0;
	char *program = argv[0];
	double time_used[3];
	inla_tp *mb = NULL;

	ncpu = inla_ncpu();
	if (ncpu > 0) {
		omp_set_num_threads(ncpu);
	} else {
		omp_set_num_threads(1);
	}
	GMRFLib_openmp = Calloc(1, GMRFLib_openmp_tp);
	GMRFLib_openmp->max_threads = IMAX(ncpu, omp_get_max_threads());
	GMRFLib_openmp->strategy = GMRFLib_OPENMP_STRATEGY_DEFAULT;
	GMRFLib_openmp_implement_strategy(GMRFLib_OPENMP_PLACES_DEFAULT, NULL);

	GMRFLib_verify_graph_read_from_disc = GMRFLib_TRUE;
	GMRFLib_collect_timer_statistics = GMRFLib_FALSE;
	GMRFLib_bitmap_max_dimension = 128;
	GMRFLib_bitmap_swap = GMRFLib_TRUE;
	GMRFLib_catch_error_for_inla = GMRFLib_TRUE;

	/*
	 * special option: if one of the arguments is `--ping', then just return INLA[<VERSION>] IS ALIVE 
	 */
	for (i = 1; i < argc; i++) {
		if (!strcasecmp(argv[i], "-ping") || !strcasecmp(argv[i], "--ping")) {
			printf("INLA[%s] IS ALIVE\n", RCSId);
			exit(EXIT_SUCCESS);
		}
	}

#if !defined(WINDOWS)
	signal(SIGUSR1, inla_signal);
	signal(SIGUSR2, inla_signal);
#endif
	while ((opt = getopt(argc, argv, "bvVe:fhist:m:S:T:N:r:FYz:cp")) != -1) {
		switch (opt) {
		case 'b':
			G.binary = 1;
			break;

		case 'v':
			silent = 1;
			verbose++;
			break;

		case 'V':
			printf("This program has version:\n\t%s\nand is linked with ", RCSId);
			GMRFLib_version(stdout);
			BUGS;
			exit(EXIT_SUCCESS);

		case 'e':
			my_setenv(optarg, 1);
			break;

		case 't':
			if (inla_sread_ints(&nt, 1, optarg) == INLA_OK) {
				GMRFLib_openmp->max_threads = IMAX(0, nt);
				omp_set_num_threads(GMRFLib_openmp->max_threads);
			} else {
				fprintf(stderr, "Fail to read MAX_THREADS from %s\n", optarg);
				exit(EXIT_SUCCESS);
			}
			break;

		case 'm':
			if (!strncasecmp(optarg, "MCMC", 4)) {
				G.mode = INLA_MODE_MCMC;
			} else if (!strncasecmp(optarg, "HYPER", 5)) {
				G.mode = INLA_MODE_HYPER;
			} else if (!strncasecmp(optarg, "QINV", 4)) {
				G.mode = INLA_MODE_QINV;
			} else if (!strncasecmp(optarg, "QSOLVE", 6)) {
				G.mode = INLA_MODE_QSOLVE;
			} else if (!strncasecmp(optarg, "QREORDERING", 11)) {
				G.mode = INLA_MODE_QREORDERING;
			} else if (!strncasecmp(optarg, "QSAMPLE", 7)) {
				G.mode = INLA_MODE_QSAMPLE;
			} else if (!strncasecmp(optarg, "FINN", 4)) {
				G.mode = INLA_MODE_FINN;
			} else if (!strncasecmp(optarg, "GRAPH", 5)) {
				G.mode = INLA_MODE_GRAPH;
			} else if (!strncasecmp(optarg, "TESTIT", 6)) {
				G.mode = INLA_MODE_TESTIT;
			} else {
				fprintf(stderr, "\n*** Error: Unknown mode (argument to '-m') : %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;

		case 'S':
			if (G.mode != INLA_MODE_MCMC) {
				fprintf(stderr, "\n *** ERROR *** Option `-S scale' only available in MCMC mode\n");
				exit(EXIT_FAILURE);
			} else {
				if (inla_sread_doubles(&G.mcmc_scale, 1, optarg) == INLA_OK) {
					;
				} else {
					fprintf(stderr, "Fail to read MCMC_SCALE from %s\n", optarg);
					exit(EXIT_SUCCESS);
				}
			}
			break;

		case 'T':
			if (G.mode != INLA_MODE_MCMC) {
				fprintf(stderr, "\n *** ERROR *** Option `-T thining' only available in MCMC mode\n");
				exit(EXIT_FAILURE);
			} else {
				if (inla_sread_ints(&G.mcmc_thinning, 1, optarg) == INLA_OK) {
					;
				} else {
					fprintf(stderr, "Fail to read MCMC_THINNING from %s\n", optarg);
					exit(EXIT_SUCCESS);
				}
			}
			break;

		case 'z':
			if (G.mode != INLA_MODE_FINN && G.mode != INLA_MODE_QSAMPLE) {
				fprintf(stderr, "\n *** ERROR *** Option `-z seed' only available in FINN mode\n");
				exit(EXIT_FAILURE);
			} else {
				int int_seed;
				if (inla_sread_ints(&int_seed, 1, optarg) == INLA_OK) {
					;
				} else {
					fprintf(stderr, "Fail to read FINN_SEED from %s\n", optarg);
					exit(EXIT_SUCCESS);
				}
				if (int_seed != 0) {
					/*
					 * this is only for the main thread...
					 */
					GMRFLib_rng_init((unsigned long int) int_seed);
				}
			}
			break;

		case 'F':
			if (G.mode != INLA_MODE_MCMC) {
				fprintf(stderr, "\n *** ERROR *** Option `-F' only available in MCMC mode\n");
				exit(EXIT_FAILURE);
			} else {
				G.mcmc_fifo = 1;
			}
			break;

		case 'Y':
			if (G.mode != INLA_MODE_MCMC) {
				fprintf(stderr, "\n *** ERROR *** Option `-Y' only available in MCMC mode\n");
				exit(EXIT_FAILURE);
			} else {
				G.mcmc_fifo_pass_data = 1;
			}
			break;

		case 'N':
			if (G.mode != INLA_MODE_MCMC) {
				fprintf(stderr, "\n *** ERROR *** Option `-N niter' only available in MCMC mode\n");
				exit(EXIT_FAILURE);
			} else {
				if (inla_sread_ints(&G.mcmc_niter, 1, optarg) == INLA_OK) {
					;
				} else {
					fprintf(stderr, "Fail to read MCMC_NITER from %s\n", optarg);
					exit(EXIT_SUCCESS);
				}
			}
			break;

		case 'h':
			HELP;
			BUGS;
			exit(EXIT_SUCCESS);
			/*
			 * some private options goes here. 
			 */

		case 's':
			verbose = 0;
			silent = 1;
			break;
		case 'f':
			GMRFLib_fpe();
			break;

		case 'i':
			GMRFLib_collect_timer_statistics = GMRFLib_TRUE;
			report = 1;
			break;

		case 'r':
			err = inla_sread_ints(&G.reorder, 1, optarg);
			if (err) {
				G.reorder = GMRFLib_reorder_id((const char *) optarg);
			}
			GMRFLib_reorder = G.reorder;	       /* yes! */
			break;

		case 'c':
			enable_core_file = 1;		       /* allow for core files */
			break;

		case 'p':
		{
#if !defined(WINDOWS)
			long int pid = (long int) getpid();
			FILE *fp_pid = fopen(".inla.pid", "w");
			if (fp_pid) {
				fprintf(fp_pid, "%ld\n", pid);
				fclose(fp_pid);
			}
#endif
		}
			break;

		default:
			USAGE;
			exit(EXIT_FAILURE);
		}
	}


#if !defined(WINDOWS)
	/*
	 * disable the creation of core-file, unless explicite asked for by the argument '-c'.
	 */
	struct rlimit rlim;
	getrlimit(RLIMIT_CORE, &rlim);
	rlim.rlim_cur = (enable_core_file ? rlim.rlim_max : (rlim_t) 0L);
	setrlimit(RLIMIT_CORE, (const struct rlimit *) &rlim);
	if (0) {
		getrlimit(RLIMIT_CORE, &rlim);
		printf("NEW cur %lld max %lld\n", (long long) rlim.rlim_cur, (long long) rlim.rlim_max);
	}
#endif

	/*
	 * these options does not belong here in this program, but it makes all easier... and its undocumented.
	 */
	if (G.mode == INLA_MODE_QINV) {
		inla_qinv(argv[optind], argv[optind + 1], argv[optind + 2]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_QSOLVE) {
		inla_qsolve(argv[optind], argv[optind + 1], argv[optind + 2], argv[optind + 3]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_QREORDERING) {
		inla_qreordering(argv[optind]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_QSAMPLE) {
		inla_qsample(argv[optind], argv[optind + 1], argv[optind + 2], argv[optind + 3], argv[optind + 4], argv[optind + 5],
			     argv[optind + 6], argv[optind + 7]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_FINN) {
		inla_finn(argv[optind]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_GRAPH) {
		inla_read_graph(argv[optind]);
		exit(EXIT_SUCCESS);
	} else if (G.mode == INLA_MODE_TESTIT) {
		testit(argc, argv);
		exit(EXIT_SUCCESS);
	} else {
		/*
		 * DO AS NORMAL...
		 */
	}

	if (!silent || verbose) {
		fprintf(stdout, "\n\t%s\n", RCSId);
	}
	if (verbose) {
		BUGS_intern(stdout);
	}
	if (verbose && G.reorder >= 0) {
		printf("Set reordering to id=[%d] and name=[%s]\n", G.reorder, GMRFLib_reorder_name(G.reorder));
	}
	if (optind >= argc) {
		fprintf(stderr, "\n*** Error: Expected argument after options.\n");
		USAGE;
		exit(EXIT_FAILURE);
	}
	if (optind < argc - 1) {
		fprintf(stderr, "\n");
		for (i = 0; i < argc; i++) {
			fprintf(stderr, "\targv[%1d] = [%s]\n", i, argv[i]);
		}
		fprintf(stderr, "\targc=[%1d] optind=[%1d]\n\n", argc, optind);
		fprintf(stderr, "\n*** Error: Can only process one .INI-file at the time.\n");
		exit(EXIT_SUCCESS);
	}
	if (verbose) {
		if (G.mode == INLA_MODE_HYPER) {
			fprintf(stderr, "\nRun in mode=[%s]\n", "HYPER");
		}
		if (G.mode == INLA_MODE_MCMC) {
			fprintf(stderr, "\nRun in mode=[%s]\n", "MCMC");
		}
	}
	if (G.mode == INLA_MODE_DEFAULT || G.mode == INLA_MODE_HYPER) {
		for (arg = optind; arg < argc; arg++) {
			if (verbose) {
				printf("Processing file [%s] max_threads=[%1d]\n", argv[arg], GMRFLib_MAX_THREADS);
			}
			if (!silent) {
				printf("\nWall-clock time used on [%s] max_threads=[%1d]\n", argv[arg], GMRFLib_MAX_THREADS);
			}
			time_used[0] = GMRFLib_cpu();
			mb = inla_build(argv[arg], verbose, 1);
			time_used[0] = GMRFLib_cpu() - time_used[0];
			if (!silent) {
				printf("\tPreparations    : %7.3f seconds\n", time_used[0]);
				fflush(stdout);
			}
			time_used[1] = GMRFLib_cpu();
			inla_INLA(mb);
			time_used[1] = GMRFLib_cpu() - time_used[1];
			if (!silent) {
				printf("\tApprox inference: %7.3f seconds\n", time_used[1]);
				fflush(stdout);
			}
			time_used[2] = GMRFLib_cpu();
			inla_output(mb);
			time_used[2] = GMRFLib_cpu() - time_used[2];
			if (!silent) {
				printf("\tOutput          : %7.3f seconds\n", time_used[2]);
				printf("\t---------------------------------\n");
				printf("\tTotal           : %7.3f seconds\n", time_used[0] + time_used[1] + time_used[2]);
				printf("\n");
				fflush(stdout);
			}
			if (verbose) {
				double tsum = mb->misc_output->wall_clock_time_used[0] +
				    mb->misc_output->wall_clock_time_used[1] + mb->misc_output->wall_clock_time_used[2] + mb->misc_output->wall_clock_time_used[3];

				printf("\nWall-clock time used on [%s]\n", argv[arg]);
				printf("\tPreparations    : %7.3f seconds\n", time_used[0]);
				printf("\tApprox inference: %7.3f seconds [%.1f|%.1f|%.1f|%.1f|%.1f]%%\n", time_used[1],
				       100 * (time_used[1] - tsum) / time_used[1],
				       100 * mb->misc_output->wall_clock_time_used[0] / time_used[1],
				       100 * mb->misc_output->wall_clock_time_used[1] / time_used[1],
				       100 * mb->misc_output->wall_clock_time_used[2] / time_used[1],
				       100 * mb->misc_output->wall_clock_time_used[3] / time_used[1]);
				printf("\tOutput          : %7.3f seconds\n", time_used[2]);
				printf("\t---------------------------------\n");
				printf("\tTotal           : %7.3f seconds\n", time_used[0] + time_used[1] + time_used[2]);
				printf("\n");
			}

			/*
			 * close fifo-pipes
			 */
			for (i = 0; i < mb->nf; i++) {
				if (mb->f_id[i] == F_R_GENERIC) {
					inla_rgeneric_tp *a = (inla_rgeneric_tp *) mb->f_Qfunc_arg[i];
					if (a) {
#pragma omp critical
						{
							WRITE_MSG(a->c2R, a->Id, R_GENERIC_QUIT);	/* this will also delete the files */
							close(a->c2R);
							close(a->R2c);
						}
						unlink(a->filename_c2R);
						unlink(a->filename_R2c);
					}
				}
			}
		}
	} else {
		assert(G.mode == INLA_MODE_MCMC);
		for (arg = optind; arg < argc; arg++) {
			inla_tp *mb_old = NULL;
			inla_tp *mb_new = NULL;
			mb_old = inla_build(argv[arg], verbose, 1);
			mb_new = inla_build(argv[arg], verbose, 0);
			inla_MCMC(mb_old, mb_new);
			inla_output_ok(mb_old->dir);
		}
	}

	if (report) {
		GMRFLib_timer_full_report(NULL);
	}

	/*
	 * final output (if not mcmc mode)
	 */
	if (mb) {
		inla_output_ok(mb->dir);
	}

	return EXIT_SUCCESS;
#undef USAGE_intern
#undef USAGE
#undef HELP
#undef BUGS_intern
#undef BUGS
}
