/*
 * This software is Copyright (c) 2018 Dhiru Kholia <kholia at kth dot se> and
 * it is hereby released to the general public under the following terms:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * Based on opencl_pfx_fmt_plug.c file, and other files which are,
 *
 * Copyright (c) 2012 Lukas Odzioba <ukasz@openwall.net>, Copyright (c) JimF,
 * and Copyright (c) magnum.
 */

#ifdef HAVE_OPENCL

#if FMT_EXTERNS_H
extern struct fmt_main fmt_opencl_ssh;
#elif FMT_REGISTERS_H
john_register_one(&fmt_opencl_ssh);
#else

#include <stdint.h>
#include <string.h>

#include "misc.h"
#include "arch.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "opencl_common.h"
#include "options.h"
#include "ssh_common.h"
#include "ssh_variable_code.h"

#define FORMAT_LABEL            "ssh-opencl"
#define FORMAT_NAME             ""
#define ALGORITHM_NAME          "RSA/DSA/EC (SSH private keys) OpenCL"
#define BENCHMARK_COMMENT       ""
#define BENCHMARK_LENGTH        0x107
#define BINARY_SIZE             0
#define BINARY_ALIGN            sizeof(uint32_t)
#define SALT_SIZE               sizeof(*cur_salt)
#define SALT_ALIGN              sizeof(int)
#define PLAINTEXT_LENGTH        32
#define MIN_KEYS_PER_CRYPT      1
#define MAX_KEYS_PER_CRYPT      1

// input
typedef struct {
	uint32_t length;
	unsigned char v[PLAINTEXT_LENGTH];
} ssh_password;

typedef struct {
	uint32_t cracked;
} ssh_out;

// input
typedef struct {
	unsigned char salt[16];
	unsigned char ct[N];
	int cipher;
	int ctl;
	int sl;
	int rounds;
	int ciphertext_begin_offset;
} ssh_salt;

static ssh_out *output;
static struct custom_salt *cur_salt;
static cl_int cl_error;
static ssh_password *inbuffer;
static ssh_salt currentsalt;
static cl_mem mem_in, mem_out, mem_setting;
static struct fmt_main *self;

size_t insize, outsize, settingsize, cracked_size;

#define STEP			0
#define SEED			256

// This file contains auto-tuning routine(s). Has to be included after formats definitions.
#include "opencl_autotune.h"

static const char *warn[] = {
	"xfer: ",  ", crypt: ",  ", xfer: "
};

/* ------- Helper functions ------- */
static size_t get_task_max_work_group_size()
{
	return autotune_get_task_max_work_group_size(FALSE, 0, crypt_kernel);
}

static void create_clobj(size_t gws, struct fmt_main *self)
{
	insize = sizeof(ssh_password) * gws;
	outsize = sizeof(ssh_out) * gws;
	settingsize = sizeof(ssh_salt);

	inbuffer = mem_calloc(1, insize);
	output = mem_alloc(outsize);

	// Allocate memory
	mem_in =
	    clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY, insize, NULL,
	    &cl_error);
	HANDLE_CLERROR(cl_error, "Error allocating mem in");
	mem_out =
	    clCreateBuffer(context[gpu_id], CL_MEM_WRITE_ONLY, outsize, NULL,
	    &cl_error);
	HANDLE_CLERROR(cl_error, "Error allocating mem out");
	mem_setting =
	    clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY, settingsize,
	    NULL, &cl_error);
	HANDLE_CLERROR(cl_error, "Error allocating mem setting");

	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 0, sizeof(mem_in),
		&mem_in), "Error while setting mem_in kernel argument");
	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 1, sizeof(mem_out),
		&mem_out), "Error while setting mem_out kernel argument");
	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 2, sizeof(mem_setting),
		&mem_setting), "Error while setting mem_salt kernel argument");
}

static void release_clobj(void)
{
	if (output) {
		HANDLE_CLERROR(clReleaseMemObject(mem_in), "Release mem in");
		HANDLE_CLERROR(clReleaseMemObject(mem_setting), "Release mem setting");
		HANDLE_CLERROR(clReleaseMemObject(mem_out), "Release mem out");

		MEM_FREE(inbuffer);
		MEM_FREE(output);
	}
}

static void init(struct fmt_main *_self)
{
	self = _self;
	opencl_prepare_dev(gpu_id);
}

static void reset(struct db_main *db)
{
	if (!autotuned) {
		char build_opts[64];

		snprintf(build_opts, sizeof(build_opts),
			 "-DPLAINTEXT_LENGTH=%d -DCTLEN=%d -DSAFETY_FACTOR=%d",
			 PLAINTEXT_LENGTH, N, 16);
		opencl_init("$JOHN/opencl/ssh_kernel.cl",
		            gpu_id, build_opts);

		crypt_kernel = clCreateKernel(program[gpu_id], "ssh", &cl_error);
		HANDLE_CLERROR(cl_error, "Error creating kernel");

		// Initialize openCL tuning (library) for this format.
		opencl_init_auto_setup(SEED, 0, NULL, warn, 1, self,
		                       create_clobj, release_clobj,
		                       sizeof(ssh_password), 0, db);

		// Auto tune execution from shared/included code.
		autotune_run(self, 1, 0, 500);
	}
}

static void done(void)
{
	if (autotuned) {
		release_clobj();

		HANDLE_CLERROR(clReleaseKernel(crypt_kernel), "Release kernel");
		HANDLE_CLERROR(clReleaseProgram(program[gpu_id]), "Release Program");

		autotuned--;
	}
}

static void set_salt(void *salt)
{
	cur_salt = (struct custom_salt*)salt;

	currentsalt.rounds = cur_salt->rounds;
	currentsalt.cipher = cur_salt->cipher;
	currentsalt.sl = cur_salt->sl;
	currentsalt.ctl = cur_salt->ctl;
	currentsalt.ciphertext_begin_offset = cur_salt->ciphertext_begin_offset;

	memcpy((char*)currentsalt.salt, cur_salt->salt, currentsalt.sl);
	memcpy((char*)currentsalt.ct, cur_salt->ct, currentsalt.ctl);

	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_setting,
		CL_FALSE, 0, settingsize, &currentsalt, 0, NULL, NULL),
	    "Copy setting to gpu");
}

static void ssh_set_key(char *key, int index)
{
	uint32_t length = strlen(key);

	if (length > PLAINTEXT_LENGTH)
		length = PLAINTEXT_LENGTH;
	inbuffer[index].length = length;
	memcpy(inbuffer[index].v, key, length);
}

static char *get_key(int index)
{
	static char ret[PLAINTEXT_LENGTH + 1];
	uint32_t length = inbuffer[index].length;

	memcpy(ret, inbuffer[index].v, length);
	ret[length] = '\0';

	return ret;
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	const int count = *pcount;
	size_t *lws = local_work_size ? &local_work_size : NULL;
	size_t gws = GET_NEXT_MULTIPLE(count, local_work_size);

	// Copy data to gpu
	BENCH_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_in, CL_FALSE, 0,
		insize, inbuffer, 0, NULL, multi_profilingEvent[0]),
		"Copy data to gpu");

	// Run kernel
	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], crypt_kernel, 1,
		NULL, &gws, lws, 0, NULL,
		multi_profilingEvent[1]),
		"Run kernel");

	// Read the result back
	BENCH_CLERROR(clEnqueueReadBuffer(queue[gpu_id], mem_out, CL_TRUE, 0, outsize, output, 0, NULL, multi_profilingEvent[5]), "Copy result back");

	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;

	for (index = 0; index < count; index++)
		if (output[index].cracked)
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	return output[index].cracked;
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

struct fmt_main fmt_opencl_ssh = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		0,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_NOT_EXACT | FMT_HUGE_INPUT,
		{
			"KDF/cipher [0=MD5/AES 1=MD5/3DES 2=Bcrypt/AES]",
			"iteration count",
		},
		{ FORMAT_TAG },
		ssh_tests
	}, {
		init,
		done,
		reset,
		fmt_default_prepare,
		ssh_valid,
		fmt_default_split,
		fmt_default_binary,
		ssh_get_salt,
		{
			ssh_kdf,
			ssh_iteration_count,
		},
		fmt_default_source,
		{
			fmt_default_binary_hash
		},
		fmt_default_salt_hash,
		NULL,
		set_salt,
		ssh_set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
			fmt_default_get_hash
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif /* plugin stanza */

#endif /* HAVE_OPENCL */
