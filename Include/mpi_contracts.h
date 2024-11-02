#pragma once
#include "Contracts.h"
#include <mpi.h>

int MPI_Finalize(void) CONTRACT(PRE {called!(MPI_Init)});
