// Copyright 2015 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef __cplusplus
extern "C"
{
#endif

#include "rcl/rcl.h"

#include <string.h>

#include "./arguments_impl.h"
#include "./stdatomic_helper.h"
#include "rcl/arguments.h"
#include "rcl/error_handling.h"
#include "rcutils/logging_macros.h"
#include "rmw/error_handling.h"

static atomic_bool __rcl_is_initialized = ATOMIC_VAR_INIT(false);
static rcl_allocator_t __rcl_allocator;
static int __rcl_argc = 0;
static char ** __rcl_argv = NULL;
static atomic_uint_least32_t __rcl_instance_id = ATOMIC_VAR_INIT(0);
static uint32_t __rcl_next_unique_id = 0;

uint_least64_t __atomic_load_8(volatile uint_least64_t* ptr, int memorder) 
{ 
    // uint_least64_t ret;
    // get_lock(ptr, memorder);
    // ret = *ptr; 
    // free_lock(ptr, memorder);
    // return ret;
    return *ptr;
}

void __atomic_store_8(volatile uint_least64_t* ptr, uint_least64_t val, int memorder) 
{
    // get_lock(ptr, memorder);
    *ptr = val;
    // free_lock(ptr, memorder);

}

uint_least64_t __atomic_exchange_8(volatile uint_least64_t* ptr, uint_least64_t val, int memorder) 
{ 
    // get_lock(ptr, memorder);
    uint_least64_t ret = *ptr;
    *ptr = val;
    // free_lock(ptr, memorder);
    return ret; 
}

static void
__clean_up_init()
{
  if (__rcl_argv) {
    int i;
    for (i = 0; i < __rcl_argc; ++i) {
      if (__rcl_argv[i]) {
        // Use the old allocator.
        __rcl_allocator.deallocate(__rcl_argv[i], __rcl_allocator.state);
      }
    }
    // Use the old allocator.
    __rcl_allocator.deallocate(__rcl_argv, __rcl_allocator.state);
  }
  __rcl_argc = 0;
  __rcl_argv = NULL;
  // This is the only place where it is OK to finalize the global arguments.
  rcl_arguments_t * global_args = rcl_get_global_arguments();
  if (NULL != global_args->impl && RCL_RET_OK != rcl_arguments_fini(global_args)) {
    rcl_reset_error();
  }
  rcl_atomic_store(&__rcl_instance_id, 0);
  rcl_atomic_store(&__rcl_is_initialized, false);
}

rcl_ret_t
rcl_init(int argc, char const * const * argv, rcl_allocator_t allocator)
{
  rcl_ret_t fail_ret = RCL_RET_ERROR;

  // Check allocator first, so it can be used in other errors.
  RCL_CHECK_ALLOCATOR_WITH_MSG(&allocator, "invalid allocator", return RCL_RET_INVALID_ARGUMENT);

  if (argc > 0) {
    RCL_CHECK_ARGUMENT_FOR_NULL(argv, RCL_RET_INVALID_ARGUMENT, allocator);
  }
  if (rcl_atomic_exchange_bool(&__rcl_is_initialized, true)) {
    RCL_SET_ERROR_MSG("rcl_init called while already initialized", allocator);
    return RCL_RET_ALREADY_INIT;
  }

  // Zero initialize global arguments before any chance of calling __clean_up_init()
  rcl_arguments_t * global_args = rcl_get_global_arguments();
  *global_args = rcl_get_zero_initialized_arguments();

  // There is a race condition between the time __rcl_is_initialized is set true,
  // and when the allocator is set, in which rcl_shutdown() could get rcl_ok() as
  // true and try to use the allocator, but it isn't set yet...
  // A very unlikely race condition, but it is possile I think.
  // I've documented that rcl_init() and rcl_shutdown() are not thread-safe with each other.
  __rcl_allocator = allocator;  // Set the new allocator.
  // Initialize rmw_init.
  rmw_ret_t rmw_ret = rmw_init();
  if (rmw_ret != RMW_RET_OK) {
    RCL_SET_ERROR_MSG(rmw_get_error_string_safe(), allocator);
    fail_ret = RCL_RET_ERROR;
    goto fail;
  }
  // TODO(wjwwood): Remove rcl specific command line arguments.
  // For now just copy the argc and argv.
  __rcl_argc = argc;
  __rcl_argv = (char **)__rcl_allocator.allocate(sizeof(char *) * argc, __rcl_allocator.state);
  if (!__rcl_argv) {
    RCL_SET_ERROR_MSG("allocation failed", allocator);
    fail_ret = RCL_RET_BAD_ALLOC;
    goto fail;
  }
  memset(__rcl_argv, 0, sizeof(char **) * argc);
  int i;
  for (i = 0; i < argc; ++i) {
    __rcl_argv[i] = (char *)__rcl_allocator.allocate(strlen(argv[i]), __rcl_allocator.state);
    if (!__rcl_argv[i]) {
      RCL_SET_ERROR_MSG("allocation failed", allocator);
      fail_ret = RCL_RET_BAD_ALLOC;
      goto fail;
    }
    memcpy(__rcl_argv[i], argv[i], strlen(argv[i]));
  }
  if (RCL_RET_OK != rcl_parse_arguments(argc, (const char **)argv, allocator, global_args)) {
    RCUTILS_LOG_ERROR_NAMED(ROS_PACKAGE_NAME, "Failed to parse global arguments");
    goto fail;
  }

  // Update the default log level if specified in arguments.
  if (global_args->impl->log_level >= 0) {
    rcutils_logging_set_default_logger_level(global_args->impl->log_level);
  }

  rcl_atomic_store(&__rcl_instance_id, ++__rcl_next_unique_id);
  uint32_t result;
  rcl_atomic_load(&__rcl_instance_id, result);
  if (result == 0) {
    // Roll over occurred.
    __rcl_next_unique_id--;  // roll back to avoid the next call succeeding.
    RCL_SET_ERROR_MSG("unique rcl instance ids exhausted", allocator);
    goto fail;
  }
  return RCL_RET_OK;
fail:
  __clean_up_init();
  return fail_ret;
}

rcl_ret_t
rcl_shutdown()
{
  RCUTILS_LOG_DEBUG_NAMED(ROS_PACKAGE_NAME, "Shutting down")
  if (!rcl_ok()) {
    // must use default allocator here because __rcl_allocator may not be set yet
    RCL_SET_ERROR_MSG("rcl_shutdown called before rcl_init", rcl_get_default_allocator());
    return RCL_RET_NOT_INIT;
  }
  __clean_up_init();
  return RCL_RET_OK;
}

uint64_t
rcl_get_instance_id()
{
  uint32_t result;
  rcl_atomic_load(&__rcl_instance_id, result);
  return result;
}

bool
rcl_ok()
{
  return rcl_atomic_load_bool(&__rcl_is_initialized);
}

#ifdef __cplusplus
}
#endif
