// RUN: %CoVerCXX %run_compile
// RUN: %run_dynamic

#include <mpi.h>

// Buffers are allocated in a member function called by the constructor, and used through the object in another function.
// Defined outside the class, the complete object constructor called by main is an alias of the base object constructor
class Comm {
public:
    Comm(int n);
    ~Comm() {
        delete[] sendBuf;
        delete[] recvBuf;
    }
    void setup(int n) {
        sendBuf = new int[n];
        recvBuf = new int[n];
    }
    int* sendBuf;
    int* recvBuf;
};

Comm::Comm(int n) { setup(n); }

void exchange(Comm& c, int rank) {
    MPI_Request req;
    if (rank == 0) {
        MPI_Isend(&c.sendBuf[0], 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &req);
    } else {
        MPI_Irecv(&c.recvBuf[0], 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &req);
    }
    MPI_Wait(&req, MPI_STATUS_IGNORE);
}

int main(int argc, char** argv) {
    int rank;
    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    Comm* c = new Comm(4);
    c->sendBuf[0] = rank;
    exchange(*c, rank);
    delete c;
    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
