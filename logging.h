#ifndef LOGGING_H_
#define LOGGING_H_
#include <iostream>

using namespace std;

inline void log_info(string msg)
{
	cout<<"INFO: "<<msg<<endl;
}

inline void log_err(string msg)
{
	cout<<"ERROR: "<<msg<<endl;
}

#endif
