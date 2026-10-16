#ifndef LACE_NC_JOBS_INTERNAL_H
#define LACE_NC_JOBS_INTERNAL_H
#include "conn/jobs.h"
void nc_job_set_owner(NcJob *j, NcOwner *o); /* takes a reference */
bool nc_job_owner_alive(const NcJob *j);     /* no owner counts as alive */
#endif
