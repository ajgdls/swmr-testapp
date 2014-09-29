#include <iostream>
#include <string>
#include <vector>
#include <assert.h>

#include <cstdlib>
#include <ctime>

#include <log4cxx/logger.h>
#include <log4cxx/xml/domconfigurator.h>
#include <log4cxx/ndc.h>
using namespace log4cxx;
using namespace log4cxx::xml;
using namespace log4cxx::helpers;


#include "hdf5.h"
#include "swmr-testdata.h"

using namespace std;

class TimeStamp {
public:
  TimeStamp();
  ~TimeStamp(){};
  void reset();
  double seconds_until_now();
  double tsdiff(timespec& start, timespec& end) const;
private:
  timespec start;
};

TimeStamp::TimeStamp ()
{
  this->start.tv_nsec = 0;
  this->start.tv_sec = 0;
  this->reset();
}

void TimeStamp::reset()
{
  clock_gettime( CLOCK_REALTIME, &this->start);
}

double TimeStamp::tsdiff (timespec& start, timespec& end) const
{
    timespec result;
    double retval = 0.0;
    /* Perform the carry for the later subtraction by updating */
    if ((end.tv_nsec-start.tv_nsec)<0) {
        result.tv_sec = end.tv_sec-start.tv_sec-1;
        result.tv_nsec = (long int)(1E9) + end.tv_nsec-start.tv_nsec;
    } else {
        result.tv_sec = end.tv_sec-start.tv_sec;
        result.tv_nsec = end.tv_nsec-start.tv_nsec;
    }
    retval = result.tv_sec;
    retval += (double)result.tv_nsec / (double)1E9;
    return retval;
}

double TimeStamp::seconds_until_now()
{
    timespec now;
    clock_gettime( CLOCK_REALTIME, &now);
    double stampdiff = this->tsdiff(this->start, now);
    return stampdiff;
}



class SWMRReader {
public:
  SWMRReader(const char * fname);
  ~SWMRReader();
  void open_file();
  void get_test_data();
  unsigned long long latest_frame_number();
  void read_latest_frame();
  bool check_dataset();
  void monitor_dataset(double timeout=2.0);

private:
  LoggerPtr log;
  string filename;
  hid_t fid;
  hsize_t dims[3];
  hsize_t maxdims[3];

  Image_t *ptestimg;
  Image_t *preadimg;
  void print_open_objects();
  double polltime;
  std::vector<bool> checks;
};

SWMRReader::SWMRReader(const char * fname) {
  this->log = Logger::getLogger("SWMRReader");
  LOG4CXX_DEBUG(log, "SWMRReader constructor. Filename: " << fname);
  this->filename = fname;
  this->fid = -1;
  this->ptestimg = NULL;
  this->preadimg = NULL;
  this->polltime = 0.2;
}

SWMRReader::~SWMRReader() {
  LOG4CXX_DEBUG(log, "SWMRReader destructor");

  if (this->fid >= 0){
    assert (H5Fclose(this->fid) >= 0);
    this->fid = -1;
  }

  if (this->ptestimg != NULL) {
    free(this->ptestimg);
    this->ptestimg = NULL;
  }
  if (this->preadimg != NULL) {
    if (this->preadimg->pdata != NULL) {
	free(this->preadimg->pdata);
	this->preadimg->pdata = NULL;
    }
    free(this->preadimg);
    this->preadimg = NULL;
  }
}

void SWMRReader::open_file() {
  // sanity check
  assert( this->filename != "" );

  /* Create file access property list */
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  assert( fapl >= 0);
  /* Set to use the latest library format */
  assert (H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) >= 0);

  this->fid = H5Fopen(this->filename.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
  assert( this->fid >= 0 );
}

void SWMRReader::get_test_data ()
{
  LOG4CXX_DEBUG(log, "Getting test data from swmr_testdata");
  this->ptestimg = (Image_t *)calloc(1, sizeof(Image_t));
  this->ptestimg->pdata = (unsigned int*)(swmr_testdata[0]);
  this->ptestimg->dims[0] = 4;
  this->ptestimg->dims[1] = 3;

  // Allocate some space for our reading-in buffer
  this->preadimg = (Image_t*)calloc(1, sizeof(Image_t));
  this->preadimg->dims[0] = this->ptestimg->dims[0];
  this->preadimg->dims[1] = this->ptestimg->dims[1];
  size_t bufsize = this->preadimg->dims[0] * this->preadimg->dims[1];
  this->preadimg->pdata = (unsigned int*)calloc(bufsize, sizeof(unsigned int));
}

unsigned long long SWMRReader::latest_frame_number()
{
  herr_t status;
  hid_t dset;
  assert( this->fid >= 0 );
  dset = H5Dopen2(fid, "data", H5P_DEFAULT);
  assert( dset >= 0 );

  /* Get the dataspace */
  hid_t dspace;
  dspace = H5Dget_space(dset);
  assert( dspace >= 0);

  /* Refresh the dataset, i.e. get the latest info from disk */
  assert( H5Drefresh(dset) >= 0);

  int ndims = H5Sget_simple_extent_ndims(dspace);
  assert( ndims == 3 );
  H5Sget_simple_extent_dims(dspace, dims, maxdims);
  /* Check the image dimensions match the test image */
  assert( this->ptestimg->dims[0] == dims[0]);
  assert( this->ptestimg->dims[1] == dims[1]);
  LOG4CXX_DEBUG(log, "Got dimensions: " << dims[0] << ", " << dims[1] << ", " << dims[2] << ", ");

  assert( H5Dclose(dset) >= 0 );
  assert( H5Sclose(dspace) >= 0 );

  return dims[2];
}

void SWMRReader::read_latest_frame()
{
  herr_t status;
  hid_t dset;
  assert( this->fid >= 0 );
  dset = H5Dopen2(fid, "data", H5P_DEFAULT);
  assert( dset >= 0 );

  /* Get the dataspace */
  hid_t dspace;
  dspace = H5Dget_space(dset);
  assert( dspace >= 0);

  hsize_t offset[3] = { 0, 0, dims[2]-1 };
  assert( offset[2] >= 0 );
  hsize_t img_size[3] = { dims[0], dims[1], 1 };
  assert( H5Sselect_hyperslab(dspace, H5S_SELECT_SET, offset, NULL, img_size, NULL) >= 0);

  hid_t memspace;
  memspace = H5Screate_simple(2,dims,NULL);
  assert(memspace >= 0);
  status = H5Sselect_hyperslab(memspace, H5S_SELECT_SET, offset, NULL, img_size, NULL);
  assert( status >= 0 );
  void * data = (void*)(this->preadimg->pdata);
  LOG4CXX_DEBUG(log, "Reading dataset");
  status = H5Dread(dset, H5T_NATIVE_UINT32, memspace, dspace, H5P_DEFAULT, data);
  assert( status >=0 );
  this->preadimg->framenumber = dims[2];

  // Cleanup
  this->print_open_objects();
  assert( H5Dclose(dset) >= 0 );
  assert( H5Sclose(dspace) >= 0 );
  assert( H5Sclose(memspace) >= 0 );
}

bool SWMRReader::check_dataset()
{
  // Sanity dimension check
  assert( this->ptestimg->dims[0] == this->preadimg->dims[0]);
  assert( this->ptestimg->dims[1] == this->preadimg->dims[1]);

  bool result = true;
  int rowlength = this->preadimg->dims[0];
  for (int x = 0; x < this->ptestimg->dims[0]; x++) {
      for (int y = 0; y < this->ptestimg->dims[1]; y++) {
	  unsigned int index = (rowlength*y)+x;
	  LOG4CXX_TRACE(log, "Comparing: (" << x << "," << y << ") "  << *(this->ptestimg->pdata + index) << " == " << *(this->preadimg->pdata + index));
	  result = *(this->ptestimg->pdata + index) == *(this->preadimg->pdata + index);
	  if (result != true) {
	      LOG4CXX_WARN(this->log, "Data mismatch. Frame = " << this->preadimg->framenumber);
	      return false;
	  }
      }
  }
  return true;
}

void SWMRReader::monitor_dataset(double timeout)
{
  bool carryon = true;
  bool check_result;
  LOG4CXX_DEBUG(log, "Starting monitoring");
  TimeStamp ts;

  while( carryon ) {
      if (this->latest_frame_number() > this->preadimg->framenumber) {
        this->read_latest_frame();
        check_result = this->check_dataset();
        this->checks.push_back(check_result);
        ts.reset();
      } else {
	double secs = ts.seconds_until_now();
	if (timeout > 0 && secs > timeout) {
	  LOG4CXX_WARN(log, "Timeout: it's been " << secs << " seconds since last read");
	  carryon=false;
	} else {
	    usleep((unsigned int)(this->polltime * 1000000));
	}
      }
  }
}


void SWMRReader::print_open_objects()
{
  LOG4CXX_TRACE(log, "    DataSets open:    " << H5Fget_obj_count( this->fid, H5F_OBJ_DATASET ));
  LOG4CXX_TRACE(log, "    Groups open:      " << H5Fget_obj_count( this->fid, H5F_OBJ_GROUP ));
  LOG4CXX_TRACE(log, "    Attributes open:  " << H5Fget_obj_count( this->fid, H5F_OBJ_ATTR ));
  LOG4CXX_TRACE(log, "    Datatypes open:   " << H5Fget_obj_count( this->fid, H5F_OBJ_DATATYPE ));
  LOG4CXX_TRACE(log, "    Files open:       " << H5Fget_obj_count( this->fid, H5F_OBJ_FILE ));
  LOG4CXX_TRACE(log, "    Sum objects open: " << H5Fget_obj_count( this->fid, H5F_OBJ_ALL ));
}

int main() {
  DOMConfigurator::configure("Log4cxxConfig.xml");
  LoggerPtr log(Logger::getLogger("main"));

  LOG4CXX_INFO(log, "Creating a SWMR Reader object");
  SWMRReader srd = SWMRReader("swmr.h5");

  LOG4CXX_INFO(log, "Opening file");
  srd.open_file();

  LOG4CXX_INFO(log, "Getting test data");
  srd.get_test_data();

  LOG4CXX_INFO(log, "Starting monitor");
  srd.monitor_dataset();

  return 0;
}

