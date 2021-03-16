/** $lic$
 * Copyright (C) 2016-2017 by The Board of Trustees of Cornell University
 * Copyright (C) 2013-2016 by The Board of Trustees of Stanford University
 *    
 * This file is part of iBench. 
 *    
 * iBench is free software; you can redistribute it and/or modify it under the
 * terms of the Modified BSD-3 License as published by the Open Source Initiative.
 *    
 * If you use this software in your research, we request that you reference
 * the iBench paper ("iBench: Quantifying Interference for Datacenter Applications", 
 * Delimitrou and Kozyrakis, IISWC'13, September 2013) as the source of the benchmark 
 * suite in any publications that use this software, and that
 * you send us a citation of your work.
 *    
 * iBench is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the BSD-3 License for more details.
 *    
 * You should have received a copy of the Modified BSD-3 License along with
 * this program. If not, see <https://opensource.org/licenses/BSD-3-Clause>.
 **/

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/param.h>
#include <numa.h>
#include <stdbool.h>

#ifdef USE_PAPI
#include <assert.h>
#include <papi.h>
#include <sys/capability.h>
#endif

#define FAKE_USE(x) (void)(x)

// Returns the size of the L3 cache in bytes, otherwise -1 on error.
int cache_size() {
    // We grab the cache size from cpu 0, assuming that all cpu caches are the
    // same size.
    // Alternatively, we could lookup the cpu we're assigned to and use that one.
    const char* cache_size_path =
        "/sys/devices/system/cpu/cpu0/cache/index3/size";

	FILE *cache_size_fd;
	if (!(cache_size_fd = fopen(cache_size_path, "r"))) {
		perror("could not open cache size file");
		return -1;
	}

	char line[512];
	if(!fgets(line, 512, cache_size_fd)) {
		fclose(cache_size_fd);
		perror("could not read from cache size file");
		return -1;
	}

	// Strip newline
	const int newline_pos = strlen(line) - 1;
	if (line[newline_pos] == '\n') {
		line[newline_pos] = '\0';
	}

	// Get multiplier
	int multiplier = 1;
	const int multiplier_pos = newline_pos - 1;
	switch (line[multiplier_pos]) {
		case 'K':
			multiplier = 1024;
		break;
		case 'M':
			multiplier = 1024 * 1024;
		break;
		case 'G':
			multiplier = 1024 * 1024 * 1024;
		break;
	}

	// Remove multiplier
	if (multiplier != 1) {
		line[multiplier_pos] = '\0';
	}

	// Line should now be clear of non-numeric characters
	int value = atoi(line);

	int cache_size = value * multiplier;

	fclose(cache_size_fd);

	return cache_size;
}

#ifdef USE_PAPI

int init_papi() {
	int eventset = PAPI_NULL;

	int retval = PAPI_library_init(PAPI_VER_CURRENT);
	if (retval != PAPI_VER_CURRENT) {
		fprintf(stderr, "Error initializing PAPI! %s\n",
				PAPI_strerror(retval));
		return PAPI_NULL;
	}

	retval = PAPI_create_eventset(&eventset);
	if (retval != PAPI_OK) {
		fprintf(stderr, "Error creating eventset! %s\n",
				PAPI_strerror(retval));
		eventset = PAPI_NULL;
	}

	// FIXME: Just to get started measuring something:
	retval = PAPI_add_named_event(eventset, "PAPI_TOT_INS");
	if (retval != PAPI_OK) {
		fprintf(stderr, "Error adding PAPI_TOT_INS: %s\n",
				PAPI_strerror(retval));
		eventset = PAPI_NULL;
	}

	return eventset;
}

void check_caps() {
	cap_t caps;
	caps = cap_get_proc();
	if (caps == NULL) {
		perror("Failed to get capabilities of current process.");
		exit(1);
	}

	char* caps_text = cap_to_text(caps, NULL);
	if (caps_text == NULL) {
		perror("Failed parse process capabilities to text.");
		exit(1);
	}
	fprintf(stderr, "INFO: Current process capabilities: %s\n", caps_text);
	if (cap_free(caps_text) == -1) {
		perror("Failed to free caps_text.");
		exit(1);
	}

	// Note: In the future this will be CAP_PERFMON instead.
	cap_value_t caps_needed[1] = { CAP_SYS_ADMIN };
	if (cap_set_flag(caps, CAP_EFFECTIVE, 2, caps_needed, CAP_SET) == -1) {
		perror("Failed to set caps flags.");
		exit(1);
	}

	if (cap_set_proc(caps) == -1) {
		perror("Failed to set process caps.");
		exit(1);
	}

	if (cap_free(caps) == -1) {
		perror("Failed to free caps.");
		exit(1);
	}
}

/*
 *	hexdump - output a hex dump of a buffer
 *
 *	fd is file descriptor to write to
 *	data is pointer to the buffer
 *	length is length of buffer to write
 *	linelen is number of chars to output per line
 *	split is number of chars in each chunk on a line
 */
int hexdump(FILE *fd, void const *data, size_t length, int linelen, int split) {
	char buffer[512];
	char *ptr;
	const void *inptr;
	int pos;
	int remaining = length;

	inptr = data;

	/*
	 *	Assert that the buffer is large enough. This should pretty much
	 *	always be the case...
	 *
	 *	hex/ascii gap (2 chars) + closing \0 (1 char)
	 *	split = 4 chars (2 each for hex/ascii) * number of splits
	 *
	 *	(hex = 3 chars, ascii = 1 char) * linelen number of chars
	 */
	assert(sizeof(buffer) >= (long unsigned int)(3 + (4 * (linelen / split)) + (linelen * 4)));

	/*
	 *	Loop through each line remaining
	 */
	while (remaining > 0) {
		int lrem;
		int splitcount;
		ptr = buffer;

		/*
		 *	Loop through the hex chars of this line
		 */
		lrem = remaining;
		splitcount = 0;
		for (pos = 0; pos < linelen; pos++) {

			/* Split hex section if required */
			if (split == splitcount++) {
				sprintf(ptr, "  ");
				ptr += 2;
				splitcount = 1;
			}

			/* If still remaining chars, output, else leave a space */
			if (lrem) {
				sprintf(ptr, "%02x ", *((unsigned char *) inptr + pos));
				lrem--;
			} else {
				sprintf(ptr, "   ");
			}
			ptr += 3;
		}

		*ptr++ = ' ';
		*ptr++ = ' ';

		/*
		 *	Loop through the ASCII chars of this line
		 */
		lrem = remaining;
		splitcount = 0;
		for (pos = 0; pos < linelen; pos++) {
			unsigned char c;

			/* Split ASCII section if required */
			if (split == splitcount++) {
				sprintf(ptr, "  ");
				ptr += 2;
				splitcount = 1;
			}

			if (lrem) {
				c = *((unsigned char *) inptr + pos);
				if (c > 31 && c < 127) {
					sprintf(ptr, "%c", c);
				} else {
					sprintf(ptr, ".");
				}
				lrem--;
				/*
				 *	These two lines would pad out the last line with spaces
				 *	which seems a bit pointless generally.
				 */
				/*
				   } else {
				   sprintf(ptr, " ");
				   */

			}
			ptr++;
		}

		*ptr = '\0';
		fprintf(fd, "%s\n", buffer);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
		inptr += linelen;
#pragma GCC diagnostic pop
		remaining -= linelen;
	}

	return 0;
}
#endif

int main(int argc, char **argv) {
	char* volatile block;

#ifdef USE_PAPI
	check_caps();

	int eventset = init_papi();
	if (eventset == PAPI_NULL) {
		fprintf(stderr, "Error initializing PAPI.");
		exit(1);
	}
#endif

	const int CACHE_SIZE = cache_size(); 
	if (CACHE_SIZE < 0) {
		fprintf(stderr, "Failed to detect cache size.");
                exit(1);
        }
	fprintf(stderr, "INFO: Detected L3 cache size: %d bytes\n", CACHE_SIZE);

	/* Usage: ./l3 <duration in sec> <intensity in percentage> */
        if (argc < 4) {
                fprintf(stderr, "Usage: ./l3 <duration in sec> <intensity in percentage> <stride>\n");
		exit(0); 
	}

        const double intensity = atof(argv[2]) / 100.0;
        if (intensity < 0 || intensity > 1) {
		fprintf(stderr, "ERROR: Invalid intensity: %f%%\n", intensity);
		exit(1);
        }

	// TODO: Ensure this is at least the cacheline size and aligned to that?
	// See Also: /sys/devices/system/cpu/cpu0/cache/index3/coherency_line_size
        const unsigned int block_size = CACHE_SIZE * intensity;

        int stride = MIN((int)block_size / 2, atoi(argv[3]));
	//fprintf(stderr, "stride: %d\n", stride);
	if (stride == -1) {
		//fprintf(stderr, "Setting stride to half the block size.\n");
		stride = block_size / 2;
	}
	else if (stride == -2) {
		//fprintf(stderr, "Setting stride to one less than the page size.\n");
		stride = getpagesize() - 1;
	}
	else if (stride <= 0) {
		fprintf(stderr, "ERROR: Invalid stride size: %d\n", stride);
		exit(1);
	}

        fprintf(stderr, "INFO: For intensity = %6.4f, block size = %u bytes, stride = %d\n", intensity, block_size, stride);

	// TODO: Eventually bind to a particular CPU/node based on CLI args.  For now, use taskset.
	numa_set_localalloc();
	//block = (char*)numa_alloc_local(block_size);
	block = (char*)mmap(NULL, block_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
	if (block == (char *) -1) {
		perror("error: failed to mmap");
		exit(1);
	};

	// Ahaha!  All of this is zeros!  Maybe we're running into the skylake zero-fill optimization?
	// See Also: https://travisdowns.github.io/blog/2020/05/13/intel-zero-opt.html
	//hexdump(stdout, block, block_size, 16, 8);
	
	unsigned long iterations = 0;
	double user_timer = atof(argv[1]);
	double time_spent = 0.0; 
  	clock_t begin, end;

	if (user_timer < 0) {
		user_timer = 0;
	}

	clock_t time_spent_clocks = 0;
	clock_t user_timer_clocks = user_timer * CLOCKS_PER_SEC;
	FAKE_USE(user_timer_clocks);

	long long papi_counter = 0; FAKE_USE(papi_counter);
	long long papi_counter_total = 0;

	int retval; FAKE_USE(retval);

	// init by filling with non-zero!

  	begin = clock();

#ifdef USE_PAPI
	PAPI_reset(eventset);
	retval = PAPI_start(eventset);
	if (retval != PAPI_OK) {
		fprintf(stderr,"Error starting PAPI: %s\n",
				PAPI_strerror(retval));
	}
#endif

	memset(block, 0x42, block_size);

	//hexdump(stdout, block, 128, 16, 8);

#ifdef USE_PAPI
	retval=PAPI_stop(eventset, &papi_counter);
	if (retval!=PAPI_OK) {
		fprintf(stderr,"Error stopping PAPI:  %s\n",
				PAPI_strerror(retval));
	}
	papi_counter_total += papi_counter;
#endif

	sched_yield();

	end = clock();

	time_spent_clocks += end - begin;

	//while (time_spent_clocks < user_timer_clocks) {
	while (iterations < 3) {
		iterations++;

		begin = clock();

#ifdef USE_PAPI
		PAPI_reset(eventset);
		retval = PAPI_start(eventset);
		if (retval != PAPI_OK) {
			fprintf(stderr,"Error starting PAPI: %s\n",
					PAPI_strerror(retval));
		}
#endif

		// TODO: Double check how this impacts the cache using perf stat.
		// FIXME: When running multiple versions of this benchmark
		// pinned to the same socket, we get an *increased* number of
		// iterations executed despite higher LLC store/load misses (?!)

		//memcpy(block, block + (block_size / 2), block_size / 2);

		// Rather than just back to front, alternate the copy.
		//memcpy(block + ((block_size / 2) * (i % 2)), block + (block_size / 2) * ((i + 1) % 2), block_size / 2);

		// Actually, after looking at the assembly, memcpy is highly
		// specialized and sometimes doesn't even use loads and just
		// does page table manipulations.  Instead, let's use an memcpy
		// stride size that's explicitly less than a typical page size.
		//size_t const stride = 2048;
		//size_t const stride = 1024;
		//size_t const stride = 512;
		//size_t const stride = 256; // avoids avx2 memmove optimizations
		//size_t const stride = 128;
		//size_t const stride = 64;
		for (int i = 0; i < block_size / 2; i += stride) {
			memcpy(block + i, (block + block_size) - (i + stride), stride);
		}

#ifdef USE_PAPI
		retval=PAPI_stop(eventset, &papi_counter);
		if (retval!=PAPI_OK) {
			fprintf(stderr,"Error stopping PAPI:  %s\n",
					PAPI_strerror(retval));
		}
		papi_counter_total += papi_counter;
#endif

		// TODO: Track how many times we were context switched out.
		// note: replaced original throttling sleep with yielding that gives chance for the workloads to run
		sched_yield(); // sleep((float)(user_timer-time_spent)/user_timer);

		end = clock();

		time_spent_clocks += end - begin;
	}

#ifdef USE_PAPI
	PAPI_cleanup_eventset(eventset);
	PAPI_destroy_eventset(&eventset);
#endif

	time_spent = time_spent_clocks / CLOCKS_PER_SEC;

	long secs = 0; // FIXME
	long nsecs = 0; // FIXME

	// Output in machine parseable format.
	fprintf(stdout, "{ \"utc_timestamp\": %ld.%ld, \"intensity\": %f, \"requested_runtime_seconds\": %f, \"l3_cache_size_bytes\": %d, \"block_size\": %u, \"iterations\": %lu, \"runtime_seconds\": %f, \"papi_counter_total\": %lld, \"papi_counter_avg\": %f }\n",
		secs, nsecs, intensity, user_timer, CACHE_SIZE, block_size, iterations, time_spent, papi_counter_total, (double)papi_counter_total/iterations);

	return 0;
}
