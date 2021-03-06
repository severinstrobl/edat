# Configuration
EDAT supports the configuration of a variety of settings in order to tune the library for the user's requirements. There are two ways of configuring EDAT - either through setting environment variables or code level configuration options at EDAT initialisation. The latter, code level configuration options, has a higher precidence than environment variables.

## Code level options

When you initialse EDAT, instead of `edatInit`, you can instead call `edatInitWithConfiguration`. The signature of this API call is `void edatInitWithConfiguration(int, char**, char**)` where the first argument is the number of options, the second argument is a pointer to strings of option keys and the third argument is a pointer to strings of values.

```c
#include "edat.h"

int main() {
  char * k[1];  
  char * v[1];
  k[0]="EDAT_REPORT_WORKER_MAPPING";
  v[0]="true";
  edatInitWithConfiguration(1, k, v);
  edatFinalise();
  return 0;
}
```

The example here illustrates the setting of a configuration options in code and this will display the mapping of workers to cores, if the programmer changes `true` to `false` and recompiles then this will be no longer reported.

## Environment variables

Alternatively the user can set environment variables to provide specific configuration options to EDAT. These are all exported via the terminal e.g.

```
export VARIABLE_NAME=VALUE
```

Environment variables have a lower precidence than (i.e. will be overridden by) code level options.

## Configuration options
In all cases the variable name and value is idenfical whether it is provided as an environment variable or code level option. For purposes of presentation we provide usage examples with environment variables.

### EDAT_NUM_WORKERS

**Value type:** An integer

**Description:** This sets the number of workers that EDAT will map tasks onto. By default the main program process is not counted in this number and hence an extra thread. The process "thread" will sleep when the *finalise* function is called. So effectively whilst the main program process is active you will have *EDAT_NUM_WORKERS + 1* active workers which will then drop down to *EDAT_NUM_WORKERS* once this has called *finalise*. 

```
export EDAT_NUM_WORKERS=12
```

Will create 12 workers which can execute tasks. Any tasks over and above this are then queued up until an idle worker becomes available.

**Default:** Number of cores reported by C++ hardware_concurrency call

### EDAT_PROGRESS_THREAD

**Value type:** A boolean

**Description:** Determines whether a background progress thread should be created to continually poll for arriving events (and hence task progress), the delivery of events and termination. If *true* then this is an extra thread, additional to the worker threads and will run continually and greedily until program termination. If it is configured not to use a background progress thread then instead an idle worker thread (when one is available) will do the polling until it is interupted by a task. In such a case there is a guarantee that if there are any idle worker threads then one of these will poll for tasks, but tasks take priority and hence there will be no polling when all workers are busy.

```
export EDAT_PROGRESS_THREAD=false
```

**Default:** true

### EDAT_REPORT_WORKER_MAPPING

**Value type:** A boolean

**Description:** Determines whether each worker will display its corresponding (local) core id at start up once worker to core mapping has been performed. This is local as it is reported within the context of a single node rather than across the system as a whole.

```
export EDAT_REPORT_WORKER_MAPPING=true
```

**Default:** false

### EDAT_WORKER_MAPPING

**Value type:** A string

**Description:** Sets the mapping (affinity) of workers to cores in the node. There are a number of possible configuration options, *auto* will allow the OS to do what it thinks is best, *linear* will go cyclically 0 to the number of cores and then wrap around if there are more workers than cores, *linearfromcore* is similar to *linear* but will start from the core ID +1 of the main process. This last option is designed when the processes are placed explicitly on the first core of a region (for instance one per NUMA region) and the rest of the cores in that region are to be workers. Note though that it does not respect this region if there are more workers than cores in the region and it will progress through into other regions and maybe even cycle through if this is the case.

```
export EDAT_WORKER_MAPPING=linear
```

**Default:** auto

### EDAT_MAIN_THREAD_WORKER

**Value type:** A boolean

**Description:** Whether the main thread will be repurposed as a worker thread once it has gone idle (called finalise in the user's code.) 

```
export EDAT_MAIN_THREAD_WORKER=true
```

**Default:** false
