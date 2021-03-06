#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "cmdline.h"

using namespace std;
using namespace sm;

#define SM_MAX_PROG_LEN 64

Cmdline::Cmdline(const std::string &progname) :
  _progname(progname), _desc(VERSION_DESC),  _version(VERSION)
{
}

Cmdline::~Cmdline(){}

void
Cmdline::change_proc_name(int argc, char **argv) {
  stringstream ss;
  ss << "[" << _progname << ":" << VERSION_ID << "]";

  int arglen = 0;
  arglen = argv[argc-1] - argv[0] + strlen(argv[argc-1]);
  if (arglen < ss.str().size()){
    return;
  }
  
  int size = snprintf ((char *)argv[0], SM_MAX_PROG_LEN,
                       ss.str().c_str());
  memset(argv[0]+size, 0, arglen - size);
}

#undef SM_MAX_PROG_LEN
