#!/bin/bash
# Set cmux's required GN product arguments on the builder's out/Release.
# PartitionAlloc-as-malloc / the allocator shim must stay off so embedded
# libghostty does not trip Chromium's "allocator loaded multiple times" abort.
# Chromium's field-trial testing config must also stay off: it enrolls local
# builds in Chrome experiments and emits X-Client-Data to Google.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && \
  awk 'function emit(name, value) { \
      if (!found[name]) print name \" = \" value; \
      found[name] = 1; \
    } \
    /^[[:space:]]*use_partition_alloc_as_malloc[[:space:]]*=/ { \
      emit(\"use_partition_alloc_as_malloc\", \"false\"); \
      next; \
    } \
    /^[[:space:]]*use_allocator_shim[[:space:]]*=/ { \
      emit(\"use_allocator_shim\", \"false\"); \
      next; \
    } \
    /^[[:space:]]*enable_backup_ref_ptr_support[[:space:]]*=/ { \
      emit(\"enable_backup_ref_ptr_support\", \"false\"); \
      next; \
    } \
    /^[[:space:]]*disable_fieldtrial_testing_config[[:space:]]*=/ { \
      emit(\"disable_fieldtrial_testing_config\", \"true\"); \
      next; \
    } \
    { print } \
    END { \
      emit(\"use_partition_alloc_as_malloc\", \"false\"); \
      emit(\"use_allocator_shim\", \"false\"); \
      emit(\"enable_backup_ref_ptr_support\", \"false\"); \
      emit(\"disable_fieldtrial_testing_config\", \"true\"); \
    }' out/Release/args.gn > out/Release/args.gn.cmux-tmp && \
  mv out/Release/args.gn.cmux-tmp out/Release/args.gn && \
  grep -Fx 'use_partition_alloc_as_malloc = false' out/Release/args.gn && \
  grep -Fx 'use_allocator_shim = false' out/Release/args.gn && \
  grep -Fx 'enable_backup_ref_ptr_support = false' out/Release/args.gn && \
  grep -Fx 'disable_fieldtrial_testing_config = true' out/Release/args.gn && \
  tail -4 out/Release/args.gn"
