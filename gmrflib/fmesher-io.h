/* fmesher-io.h
 * 
 * Copyright (C) 2010-2011 Havard Rue
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
#ifndef __FMESHER_IO_H__
#define __FMESHER_IO_H__
#undef __BEGIN_DECLS
#undef __END_DECLS
#ifdef __cplusplus
# define __BEGIN_DECLS extern "C" {
# define __END_DECLS }
#else
# define __BEGIN_DECLS					       /* empty */
# define __END_DECLS					       /* empty */
#endif
__BEGIN_DECLS


/* 
   this follows std (i,j,values) storage. in case of dense matrix, then the values are stored in A, columnwise. 
 */
typedef struct
{
	int nrow;
	int ncol;

	/* 
	   sparse
	*/
	int elems;					       /* number of elements */
	int *i;
	int *j;

	/* 
	   on reading; 'values' are always set. on writing; only one of these can be set. 
	*/
	int *ivalues;
	double *values;

	/* 
	   dense, columnwise. on reading `A' is always set. on writing; only one of these can be set. 
	*/
	int *iA;
	double *A;

	/* 
	   if read from file, add the fileinfo here
	*/
	char *filename;					       /* filename if any, where this file is read from */
	long int offset;				       /* offset in the file */
	int whence;					       /* whence of the file */
	long int tell;					       /* the position where this matrix ended */
}
	GMRFLib_matrix_tp;


GMRFLib_matrix_tp *GMRFLib_matrix_1(int n);
GMRFLib_matrix_tp *GMRFLib_read_fmesher_file(const char *filename, long int offset, int whence);
double *GMRFLib_matrix_get_diagonal(GMRFLib_matrix_tp *M);
int GMRFLib_file_exists(const char *filename, const char *mode);
int GMRFLib_is_fmesher_file(const char *filename,  long int offset,  int whence);
int GMRFLib_matrix_free(GMRFLib_matrix_tp *M);
int GMRFLib_write_fmesher_file(GMRFLib_matrix_tp *M, const char *filename, long int offset, int whence);

__END_DECLS

#endif