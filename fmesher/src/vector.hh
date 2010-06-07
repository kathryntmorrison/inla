#ifndef _FMESH_VECTOR_
#define _FMESH_VECTOR_ 1

#include <cstddef>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <string>
#include <cmath>

#ifndef NOT_IMPLEMENTED
#define NOT_IMPLEMENTED (std::cout					\
			 << __FILE__ << "(" << __LINE__ << ")\t"	\
			 << "NOT IMPLEMENTED: "				\
			 << __PRETTY_FUNCTION__ << std::endl);
#endif

namespace fmesh {



  template <class T> class Matrix;
  template <class T> class Vector3;
  template <class T> class Matrix3;
  template <class T> class SparseMatrixRow;
  template <class T> class SparseMatrix;

  template <class T>
  SparseMatrix<T> operator*(const SparseMatrix<T>& M1,
			    const SparseMatrix<T>& M2);
  template <class T>
  SparseMatrix<T> operator-(const SparseMatrix<T>& M1,
			    const SparseMatrix<T>& M2);
  template <class T>
  SparseMatrix<T> inverse(const SparseMatrix<T>& M1,
			  bool diagonal = false);



  template<class T>
  std::ostream& operator<<(std::ostream& output,
			   const Matrix<T>& M);
  template<class T>
  std::ostream& operator<<(std::ostream& output,
			   const SparseMatrix<T>& M);


  template <class T>
  class Matrix {
    friend
    std::ostream& operator<< <> (std::ostream& output,
				 const Matrix<T>& M);
    
  protected:
    static const size_t capacity_step_size_ = 1024;
    static const size_t capacity_doubling_limit_ = 8192;
    static const T zero_;
    T* data_;
    size_t rows_;
    size_t cols_;
    size_t cap_;
  public:
    Matrix() : data_(NULL), rows_(0), cols_(0), cap_(0) {};
    Matrix(size_t set_cols) : data_(NULL), rows_(0), cols_(0),
			      cap_(0) {
      cols(set_cols);
    };
    Matrix(size_t set_rows, size_t set_cols, const T* vals = NULL)
      : data_(NULL), rows_(0), cols_(0), cap_(0) {
      cols(set_cols);
      capacity(set_rows);
      rows_ = set_rows;
      if (vals) {
	std::memcpy(data_,vals,sizeof(T)*rows_*cols_);
      }
    };
    const Matrix<T>& operator=(const Matrix<T>& from) {
      clear();
      cols(from.cols_);
      capacity(from.cap_);
      rows_ = from.rows_;
      if (data_) {
	std::memcpy(data_,from.data_,sizeof(T)*rows_*cols_);
      }
      return *this;
    };
    ~Matrix() {
      if (data_)
	delete[] data_;
    };
    void clear(void) {
      if (data_) {
	delete[] data_;
	data_ = NULL;
      }
      cap_ = 0;
      rows_ = 0;
      cols_ = 0;
    };
    void zeros(const size_t from_row = 0) {
      for (size_t i=from_row*cols_; i<cap_*cols_; i++)
	data_[i] = zero_;
    }
    void truncate(const size_t new_rows) {
      if (new_rows < rows_)
	rows_ = new_rows;
    }

    size_t capacity() const { return cap_; };
    bool capacity(size_t cap) {
      if (cap <= cap_) {
	return true;
      }
      size_t old_cap = cap_;
      if ((cap_==0) && 
	  (cap < capacity_step_size_))
	cap_ = cap;
      while (cap > cap_) {
	if (cap_ < capacity_step_size_)
	  cap_ = capacity_step_size_;
	else {
	  if (cap_ < capacity_doubling_limit_)
	    cap_ *= 2;
	  else
	    cap_ += capacity_step_size_;
	}
      }

      T* data_new_ = new T[cap_*cols_];

      if (data_) { /* Copy existing data: */
	std::memcpy(data_new_,data_,
		    sizeof(T)*old_cap*cols_);
	delete[] data_;
      }

      data_ = data_new_;
      zeros(old_cap);
      return true;
    };

    bool append(const Matrix<T>& toappend) {
      if (cols_ != toappend.cols_) return false;
      if (!capacity(rows_+toappend.rows_)) return false;
      std::memcpy(data_+rows_*cols_,toappend.data_,
		  sizeof(T)*toappend.rows_*cols_);
      rows_ += toappend.rows_;
      return true;
    };

    int rows(size_t set_rows) {
      if (set_rows>rows_)
	capacity(set_rows);
      else if (set_rows<rows_)
	truncate(set_rows);
      return rows_;
    };
    int rows(void) const { return rows_; };
    int cols(void) const { return cols_; };
    int cols(size_t set_cols) {
      /* When set, cannot alter number of columns,
	 unless we only need to expand a single row. */
      if ((cols_ > 0) &&
	  (!((rows_<=1) && cols_<=set_cols))) {
	return cols_;
      }
      if ((cols_>0) && (rows_>0)) { /* We already have some data, and
				       need to carefully make sure the
				       capacity is enough */
	/* Pre-data-size: cap_*cols_
	   Post-data_size needed cap_*set_cols
	   capacity is set as r*cols_
	   Requirement: r*cols_ >= cap_*set_cols
	   r = (cap_*set_cols)/cols_+1
	 */
	capacity((cap_*set_cols)/cols_+1);
	cols_ = set_cols;
	cap_ = rows_; /* This makes sure we dont' overestimate the true cap_. */
      } else
	cols_ = set_cols;
      return cols_;
    };

    const T (* operator[](const int r) const) {
      if (r >= (int)rows_) {
	return NULL;
      }
      return &data_[r*cols_];
    };

    T* operator()(const int r) {
      if (r >= (int)rows_) {
	capacity(r+1);
	rows_ = r+1;
      }
      return &data_[r*cols_]; 
    };

    T& operator()(const int r, const int c) {
      if (c >= (int)cols_)
	cols(c+1);
      return operator()(r)[c]; 
      /*
      if (r >= (int)rows_) {
	capacity(r+1);
	rows_ = r+1;
      }
      return data_[r*cols_+c]; 
      */
    };

    const T& operator()(const int r, const int c, const T& val) {
      operator()(r,c) = val; 
      return data_[r*cols_+c]; 
    };

    const T (* raw(void) const) { return data_; }
    T* raw(void) { return data_; }

  };


  template <class T>
  class Vector3 {
  public:
    typedef Vector3<T> selfT;
    typedef T Raw[3];
  private:
    T s[3];
  public:
    Vector3() {
      s[0] = T();
      s[1] = T();
      s[2] = T();
    };
    Vector3(const T& val0,
	    const T& val1,
	    const T& val2) {
      s[0] = val0;
      s[1] = val1;
      s[2] = val2;
    };
    Vector3(const T val[3]) {
      s[0] = val[0];
      s[1] = val[1];
      s[2] = val[2];
    };
    Vector3(const selfT& vec) {
      s[0] = vec.s[0];
      s[1] = vec.s[1];
      s[2] = vec.s[2];
    };

    selfT& operator=(const selfT vec) {
      return copy(vec);
    };

    const T& operator[](const int i) const {
      return s[i];
    };

    T& operator[](const int i) {
      return s[i];
    };

    const Raw& raw(void) const { return s; }
    


    selfT& copy(const selfT& s0)
    {
      if (this != &s0) {
	s[0] = s0.s[0];
	s[1] = s0.s[1];
	s[2] = s0.s[2];
      }
      return *this;
    };
    selfT& rescale(T s1)
    {
      s[0] *= s1;
      s[1] *= s1;
      s[2] *= s1;
      return *this;
    };
    selfT& scale(const selfT& s0, T s1)
    {
      s[0] = s0.s[0]*s1;
      s[1] = s0.s[1]*s1;
      s[2] = s0.s[2]*s1;
      return *this;
    };
    selfT& diff(const selfT& s0, const selfT& s1)
    {
      s[0] = s0.s[0]-s1.s[0];
      s[1] = s0.s[1]-s1.s[1];
      s[2] = s0.s[2]-s1.s[2];
      return *this;
    };
    selfT& sum(const selfT& s0, const selfT& s1)
    {
      s[0] = s0.s[0]+s1.s[0];
      s[1] = s0.s[1]+s1.s[1];
      s[2] = s0.s[2]+s1.s[2];
      return *this;
    };
    selfT& accum(const selfT& s0, T s1)
    {
      s[0] += s0.s[0]*s1;
      s[1] += s0.s[1]*s1;
      s[2] += s0.s[2]*s1;
      return *this;
    };
    T scalar(const selfT& s1) const
    {
      return (s[0]*s1.s[0]+s[1]*s1.s[1]+s[2]*s1.s[2]);
    };
    double length() const;
    selfT& cross(const selfT& s0,const selfT& s1)
    {
      if ((this == &s0) || (this == &s0)) {
	T s_0 = s0.s[1]*s1.s[2]-s0.s[2]*s1.s[1];
	T s_1 = s0.s[2]*s1.s[0]-s0.s[0]*s1.s[2];
	T s_2 = s0.s[0]*s1.s[1]-s0.s[1]*s1.s[0];
	s[0] = s_0;
	s[1] = s_1;
	s[2] = s_2;
      } else {
	s[0] = s0.s[1]*s1.s[2]-s0.s[2]*s1.s[1];
	s[1] = s0.s[2]*s1.s[0]-s0.s[0]*s1.s[2];
	s[2] = s0.s[0]*s1.s[1]-s0.s[1]*s1.s[0];
      }
      return *this;
    };
    T cross2(const selfT& s1) const
    {
      return (s[0]*s1.s[1]-s[1]*s1.s[0]);
    };
    /*!
      "Volume product" = scalar(s0,cross(s1,s2))
     */
    T volume(const selfT& s1, const selfT& s2) const
    {
      return ((s1.s[1]*s2.s[2]-s1.s[2]*s2.s[1])*s[0]+
	      (s1.s[2]*s2.s[0]-s1.s[0]*s2.s[2])*s[1]+
	      (s1.s[0]*s2.s[1]-s1.s[1]*s2.s[0])*s[2]);
    };
    double angle(const selfT& s1) const
    {
      Vector3<T> s0xs1;
      s0xs1.cross(*this,s1);
      return std::atan2((double)s0xs1.length(),(double)scalar(s1));
    };

    friend
    void arbitrary_perpendicular(Vector3<double>& n,
				 const Vector3<double>& v);
  };



  template <class T>
  class Matrix1 : public Matrix<T> {
  public:
    typedef T ValueRaw;
    Matrix1() : Matrix<T>(1) {};
    Matrix1(size_t set_rows, const ValueRaw* vals = NULL)
      : Matrix<T>(set_rows,1,(T*)vals) {};
    void clear(void) {
      Matrix<T>::clear();
      Matrix<T>::cols(1);
    };

    const T& operator[](const int r) const {
      if (r >= (int)Matrix<T>::rows_) {
	/* ERROR */
      }
      return Matrix<T>::data_[r];
    };

    T& operator()(const int r) {
      return Matrix<T>::operator()(r,0);
    };
    
  };


  template <class T>
  class Matrix3 : public Matrix<T> {
  public:
    typedef T ValueRaw[3];
    typedef Vector3<T> ValueRow;
    Matrix3() : Matrix<T>(3) {};
    Matrix3(const Matrix<T>& M) : Matrix<T>(3) {
      for (int r=0; r < M.rows(); r++) {
	for (int c=0; (c < 3) && (c < M.cols()); c++) {
	  operator()(r,c,M[r][c]);
	}
      }
    };
    Matrix3(size_t set_rows, const ValueRow* vals)
      : Matrix<T>(set_rows,3,(T*)vals) {};
    Matrix3(size_t set_rows, const ValueRaw* vals = NULL)
      : Matrix<T>(set_rows,3,(T*)vals) {};
    void clear(void) {
      Matrix<T>::clear();
      Matrix<T>::cols(3);
    };

    const ValueRow& operator[](const int r) const {
      return ((ValueRow*)Matrix<T>::data_)[r];
    };
    
    ValueRow& operator()(const int r) {
      return *(ValueRow*)(&Matrix<T>::operator()(r,0));
    };
    
    T& operator()(const int r, const int c) {
      return Matrix<T>::operator()(r,c);
    };
    
    const T& operator()(const int r, const int c, const T& val) {
      return Matrix<T>::operator()(r,c,val);
    };

  };


  typedef Matrix3<double>::ValueRaw PointRaw;
  typedef Matrix3<int>::ValueRaw Int3Raw;
  typedef Matrix3<double>::ValueRow Point;
  typedef Matrix3<int>::ValueRow Int3;
  typedef Matrix1<int> Matrix1int;
  typedef Matrix1<double> Matrix1double;
  typedef Matrix3<int> Matrix3int;
  typedef Matrix3<double> Matrix3double;




  template <class T>
  class SparseMatrixTriplet {
  public:
    int r;
    int c;
    T value;
    SparseMatrixTriplet()
      : r(0), c(0), value(T()) {};
    SparseMatrixTriplet(int set_r, int set_c, const T& set_value)
      : r(set_r), c(set_c), value(set_value) {};
  };




  template <class T>
  class SparseMatrixRow {
    friend class SparseMatrix<T>;
  public:
    typedef typename std::map<int, T> DataType;
    typedef typename DataType::const_iterator ColCIter;
    typedef typename DataType::const_reverse_iterator ColCRIter;
    typedef typename DataType::iterator ColIter;
    typedef typename DataType::reverse_iterator ColRIter;
  protected:
    static const T zero_;
    SparseMatrix<T>* M_;
    DataType data_;
  public:
    SparseMatrixRow()
      : M_(NULL), data_() { };
    SparseMatrixRow(const SparseMatrixRow<T>& from)
      : M_(from.M_), data_(from.data_) { };
    SparseMatrixRow(SparseMatrix<T>* M)
      : M_(M), data_() { };

    int size() const { return data_.size(); };

    void cols(size_t set_cols) {
      ColRIter col;
      if (data_.size()>0) {
	for (col = data_.rbegin();
	     (col != data_.rend()) && (!(col->first < (int)set_cols));
	     col = data_.rbegin()) {
	  data_.erase((++col).base());
	}
      }
    };

    int nnz(int r, int matrixt = 0) const {
      int nnz_ = 0;
      if (matrixt == 2) {
	ColCIter col;
	if ((col = data_.find(r)) != data_.end()) {
	  nnz_ = 1;
	}
      } else if (matrixt == 1) {
	for (ColCIter c = data_.begin();
	     c != data_.end();
	     c++)
	  if (r <= c->first) nnz_++;
      } else {
	nnz_ = data_.size();
      }
      return nnz_;
    };

    int tolist(int offset,
	       int row,
	       Matrix1< SparseMatrixTriplet<T> >& MT,
	       int matrixt = 0) const {
      int elem = 0;
      if (matrixt == 2) {
	ColCIter col;
	if ((col = data_.find(row)) != data_.end()) {
	  MT(offset+elem) = SparseMatrixTriplet<T>(row,row,
					    col->second);
	  elem++;
	}
      } else {
	for (ColCIter col = data_.begin();
	     col != data_.end();
	     col++) {
	  if ((matrixt==0) ||
	      (row <= col->first)) {
	    MT(offset+elem) = SparseMatrixTriplet<T>(row,col->first,
						     col->second);
	    elem++;
	  }
	}
      }
      return elem;
    };
    /*! To list, general, symmetric, or diagonal. */
    int tolist(int offset,
	       int row,
	       Matrix1< int >& Tr,
	       Matrix1< int >& Tc,
	       Matrix1< T >& Tv,
	       int matrixt = 0) const {
      int elem = 0;
      if (matrixt==2) {
	ColCIter col;
	if ((col = data_.find(row)) != data_.end()) {
	  Tr(offset+elem) = row;
	  Tc(offset+elem) = row;
	  Tv(offset+elem) = col->second;
	  elem++;
	}
      } else {
	for (ColCIter col = data_.begin();
	     col != data_.end();
	     col++) {
	  if ((matrixt==0) ||
	      (row <= col->first)) {
	    Tr(offset+elem) = row;
	    Tc(offset+elem) = col->first;
	    Tv(offset+elem) = col->second;
	    elem++;
	  }
	}
      }
      return elem;
    };

    ColIter begin() { return data_.begin(); };
    ColRIter rbegin() { return data_.rbegin(); };
    ColIter end() { return data_.end(); };
    ColRIter rend() { return data_.rend(); };
    ColIter find(int c) { return data_.find(c); };

  public:

    ColCIter begin() const { return data_.begin(); };
    ColCRIter rbegin() const { return data_.rbegin(); };
    ColCIter end() const { return data_.end(); };
    ColCRIter rend() const { return data_.rend(); };
    ColCIter find(int c) const { return data_.find(c); };

    const T& operator[](const int c) const {
      if (!(c < M_->cols())) {
	/* Range error. */
	return zero_;
      }
      ColCIter col;
      if ((col = data_.find(c)) != data_.end())
	return col->second;
      return zero_;
    };
    
    T& operator()(const int c) {
      if (!(c < M_->cols()))
	M_->cols(c+1);
      return data_[c];
    };

    void erase(ColIter& col) {
      data_.erase(col);
    };
    
    void erase(int c) {
      ColIter col;
      if ((col = data_.find(c)) != data_.end())
	data_.erase(col);
    };
    
  };


  template <class T>
  class SparseMatrix {
    friend
    std::ostream& operator<< <> (std::ostream& output,
				 const SparseMatrix<T>& M);
    
  public:
    typedef typename fmesh::SparseMatrixRow<T> RowType;
    typedef typename std::vector<RowType> DataType;
    typedef typename RowType::ColCIter ColCIter;
    typedef typename RowType::ColCRIter ColCRIter;
    typedef typename RowType::ColIter ColIter;
    typedef typename RowType::ColRIter ColRIter;

  private:
    static const T zero_;
    size_t cols_;
    DataType data_;
  public:
    SparseMatrix(size_t set_rows = 0, size_t set_cols = 0)
      : cols_(set_cols), data_() {
      rows(set_rows);
    };
    SparseMatrix(const SparseMatrix<T>& from)
      : cols_(from.cols_), data_(from.data_) {
      for (int r=0; r<rows(); r++) {
	data_[r].M_ = this;
      }
      //      std::cout << "SM copy" << std::endl;
    };
    const SparseMatrix<T>& operator=(const SparseMatrix<T>& from) {
      cols_ = from.cols_;
      data_ = from.data_;
      for (int r=0; r<rows(); r++) {
	data_[r].M_ = this;
      }
      //      std::cout << "SM assignment" << std::endl;
      return *this;
    };
    SparseMatrix<T>& clear() {
      data_.clear();
      return *this;
    };

    SparseMatrix<T>& rows(size_t set_rows) {
      data_.resize(set_rows, RowType(this));
      return *this;
    };

    SparseMatrix<T>& cols(size_t set_cols) {
      if (!(cols_<set_cols)) {
	for (int row=0; row<rows(); row++) {
	  data_[row].cols(set_cols);
	}
      }
      cols_ = set_cols;
      return *this;
    };

    int rows(void) const {
      return data_.size();
    };

    int cols(void) const {
      return cols_;
    };

    int nnz(int matrixt = 0) const {
      int nnz_ = 0;
      for (int row=0; row<rows(); row++)
	nnz_ += data_[row].nnz(row,matrixt);
      return nnz_;
    };

    bool non_zero(const int r,  const int c) const {
      if (r < rows()) {
	return (data_[r].find(c) != data_[r].end());
      } else {
	return false; 
      }
    };

    const T& operator()(const int r,  const int c) const {
      if (r<rows()) {
	return data_[r][c];
      } else {
	return zero_; 
      }
    };

    const RowType& operator[](const int r) const {
      if (!(r<rows())) {
	/* Range error. */
      }
      return data_[r];
    };

    RowType& operator()(const int r) {
      if (!(r<rows()))
	rows(r+1); /* Expand. */
      return data_[r];
    };

    T& operator()(const int r,  const int c) {
      return operator()(r)(c);
    };

    const T& operator()(const int r,  const int c, const T& val) {
      if (val == zero_) {
	if (r < rows()) {
	  data_[r].erase(r);
	}
	return zero_;
      } else {
	return (operator()(r)(c) = val);
      }
    };


    /* Linear algebra */
    friend
    SparseMatrix<T> operator*<T>(const SparseMatrix<T>& M1,
				 const SparseMatrix<T>& M2);
    friend
    SparseMatrix<T> operator-<T>(const SparseMatrix<T>& M1,
				 const SparseMatrix<T>& M2);
    friend
    SparseMatrix<T> inverse<T>(const SparseMatrix<T>& M1,
			       bool diagonal);


    /*! To list, general, symmetric, or diagonal. */
    int tolist(Matrix1< SparseMatrixTriplet<T> >& MT,
	       int matrixt = 0) const {
      int elem = 0;
      for (int row=0; row<rows(); row++) {
	elem += data_[row].tolist(elem,row,MT,matrixt);
      }
      return elem;
    };
    /*! To list, general, symmetric, or diagonal. */
    int tolist(Matrix1< int >& Tr,
	       Matrix1< int >& Tc,
	       Matrix1< T >& Tv,
	       int matrixt = 0) const {
      int elem = 0;
      for (int row=0; row < rows(); row++)
	elem += data_[row].tolist(elem,row,Tr,Tc,Tv,matrixt);
      return elem;
    };

    /*! From list, general, symmetric, or diagonal. */
    void fromlist(const Matrix1< SparseMatrixTriplet<T> >& MT,
		  int matrixt = 0) {
      if (matrixt==1) {
	for (int i=0; i<MT.rows(); i++) {
	  operator()(MT[i].r,MT[i].c,MT[i].value);
	  operator()(MT[i].c,MT[i].r,MT[i].value);
	}
      } else if (matrixt==2) {
	for (int i=0; i<MT.rows(); i++)
	  operator()(MT[i].r,MT[i].r,MT[i].value);
      } else {
	for (int i=0; i<MT.rows(); i++)
	  operator()(MT[i].r,MT[i].c,MT[i].value);
      }
    };
    /*! From list, general or symmetric. */
    void fromlist(const Matrix1< int >& Tr,
		  const Matrix1< int >& Tc,
		  const Matrix1< T >& Tv,
		  int matrixt = 0) {
      if (matrixt==1) {
	for (int i=0; i<Tr.rows(); i++) {
	  operator()(Tr[i],Tc[i],Tv[i]);
	  operator()(Tc[i],Tr[i],Tv[i]);
	}
      } else if (matrixt==1) {
	for (int i=0; i<Tr.rows(); i++) {
	  operator()(Tr[i],Tr[i],Tv[i]);
	}
      } else {
	for (int i=0; i<Tr.rows(); i++) {
	  operator()(Tr[i],Tc[i],Tv[i]);
	}
      }
    };
    
  };


  struct Vec {  
    static void copy(Point& s, const Point& s0)
    { s.copy(s0); };
    static void rescale(Point& s, double s1)
    { s.rescale(s1); };
    static void scale(Point& s, const Point& s0, double s1)
    { s.scale(s0,s1); };
    static void diff(Point& s,const Point& s0, const Point& s1)
    { s.diff(s0,s1); };
    static void sum(Point& s,const Point& s0, const Point& s1)
    { s.sum(s0,s1); };
    static void accum(Point& s, const Point& s0, double s1 = 1.0)
    { s.accum(s0,s1); };
    static double scalar(const Point& s0, const Point& s1)
    { return s0.scalar(s1); };
    static double length(const Point& s0);
    static void cross(Point& s, const Point& s0, const Point& s1)
    { s.cross(s0,s1); };
    static double cross2(const Point& s0, const Point& s1)
    { return s0.cross2(s1); };
    static double volume(const Point& s0, const Point& s1, const Point& s2)
    { return s0.volume(s1,s2); };
    static double angle(const Point& s0, const Point& s1)
    { return s0.angle(s1); };
    /*!
      Calculate an arbitrary perpendicular vector.

      Michael M. Stark, Efficient Construction of Perpendicular
      Vectors without Branching, Journal of graphics, gpu, and game
      tools, Vol. 14, No. 1: 55-62, 2009
    */
#define ABS(X) std::fabs(X)
#define SIGNBIT(X) ((unsigned int)(std::signbit(X) != 0))
    static void arbitrary_perpendicular(Point& n, const Point& v)
    {
      const unsigned int uyx = SIGNBIT(ABS(v[0]) - ABS(v[1]));
      const unsigned int uzx = SIGNBIT(ABS(v[0]) - ABS(v[2]));
      const unsigned int uzy = SIGNBIT(ABS(v[1]) - ABS(v[2]));
      const unsigned int xm = uyx & uzx;
      const unsigned int ym = (1^xm) & uzy;
      const unsigned int zm = 1^(xm & ym);
      std::cout << uyx << ' ' << uzx << ' ' << uzy << std::endl;
      std::cout << xm << ' ' << ym << ' ' << zm << std::endl;
      n[0] =  zm*v[1] - ym*v[2];
      n[1] =  xm*v[2] - zm*v[0];
      n[2] =  ym*v[0] - xm*v[1];
    };
  };

  template <class T>
  double Vector3<T>::length() const
  {
    return (std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]));
  };




  template<class T>
  std::ostream& operator<<(std::ostream& output,
			   const Matrix<T>& M)
  {
    output << M.rows_ << " "
	   << M.cols_ << std::endl;
    for (int r=0; r<M.rows(); r++) {
      for (int c=0; c<M.cols(); c++) {
	output << M.data_[r*M.cols()+c] << " ";
      }
      output << std::endl;
    }
    return output;
  }


  template<class T>
  std::ostream& operator<<(std::ostream& output,
			   const SparseMatrixTriplet<T>& MT)
  {
    output << MT.r << " "
	   << MT.c << " "
	   << MT.value;
    return output;
  }

  template<class T>
  std::istream& operator>>(std::istream& input,
			   SparseMatrixTriplet<T>& MT)
  {
    input >> MT.r
	  >> MT.c
	  >> MT.value;
    return input;
  }


  template<class T>
  std::ostream& operator<<(std::ostream& output,
			   const SparseMatrix<T>& M)
  {
    output << M.rows() << " "
	   << M.cols() << " "
	   << M.nnz() << std::endl;
    for (int row=0; row<M.rows(); row++)
      for (typename SparseMatrix<T>::ColCIter col
	     = M[row].begin();
	   col != M[row].end();
	   col++) {
	output << row << " "
	       << col->first << " "
	       << col->second << std::endl;
      }
    return output;
  }




  template <class T>
  SparseMatrix<T> operator*(const SparseMatrix<T>& M1,
			    const SparseMatrix<T>& M2)
  {
    SparseMatrix<T> M;
    int M1rows = M1.rows();
    int M2rows = M2.rows();
    M.cols(M2.cols()).rows(M1rows);
    for (int i=0; i<M1rows; i++) {
      SparseMatrixRow<T>& Mi = M(i);
      const SparseMatrixRow<T>& M1i = M1[i];
      if (M1i.size() > 0) {
	for (typename SparseMatrixRow<T>::ColCIter M1k = M1i.begin();
	     (M1k != M1i.end()) && (M1k->first < M2rows);
	     M1k++) {
	  int k = M1k->first;
	  const T& M1ik = M1i[k];
	  const SparseMatrixRow<T>& M2k = M2[k];
	  for (typename SparseMatrixRow<T>::ColCIter M2j = M2k.begin();
	       (M2j != M2k.end());
	       M2j++) {
	    Mi(M2j->first) += (M1ik * M2j->second);
	  }
	}
      }
    }
    return M;
  }

  template <class T>
  SparseMatrix<T> operator-(const SparseMatrix<T>& M1,
			    const SparseMatrix<T>& M2)
  {
    SparseMatrix<T> M(M1);
    for (int r=0; (r<M1.rows()) && (r<M2.rows()); r++) {
      SparseMatrixRow<T>& Mr = M(r);
      const SparseMatrixRow<T>& M2r = M2[r];
      for (typename SparseMatrixRow<T>::ColCIter c = M2r.begin();
	   (c != M2r.end()) && (c->first < M1.cols());
	   c++) {
	Mr(c->first) -= c->second;
      }
    }
    return M;
  }

  template <class T>
  SparseMatrix<T> inverse(const SparseMatrix<T>& M1,
			  bool diagonal)
  {
    SparseMatrix<T> M;
    M.cols(M1.cols()).rows(M1.rows());
    if (!diagonal) {
      /* NOT IMPLEMENTED */
      return M;
    }
    for (int r=0; (r<M1.rows()) && (r<M1.cols()); r++) {
      const T& val = M1[r][r];
      if (!(val==T()))
	M(r,r) = 1/val;
    }
    return M;
  }






  template <class T>
  const T fmesh::Matrix<T>::zero_ = T();
  template <class T>
  const T fmesh::SparseMatrixRow<T>::zero_ = T();
  template <class T>
  const T fmesh::SparseMatrix<T>::zero_ = T();


} /* namespace fmesh */

#endif
