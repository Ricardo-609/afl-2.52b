/*
   american fuzzy lop - wrapper for GCC and clang
   ----------------------------------------------

   Written and maintained by Michal Zalewski <lcamtuf@google.com>

   Copyright 2013, 2014, 2015 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This program is a drop-in replacement for GCC or clang. The most common way
   of using it is to pass the path to afl-gcc or afl-clang via CC when invoking
   ./configure.

   (Of course, use CXX and point it to afl-g++ / afl-clang++ for C++ code.)

   The wrapper needs to know the path to afl-as (renamed to 'as'). The default
   is /usr/local/lib/afl/. A convenient way to specify alternative directories
   would be to set AFL_PATH.

   If AFL_HARDEN is set, the wrapper will compile the target app with various
   hardening options that may help detect memory management issues more
   reliably. You can also specify AFL_USE_ASAN to enable ASAN.

   If you want to call a non-default compiler as a next step of the chain,
   specify its location via AFL_CC or AFL_CXX.

 */
/*
  afl-gcc 的主要作用是实现对于关键节点的代码插桩，属于汇编级，从而记录程序执行路径之类的关键信息，对程序的运行情况进行反馈
*/
#define AFL_MAIN

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static u8*  as_path;                /* Path to the AFL 'as' wrapper      AFL的as路径*/
static u8** cc_params;              /* Parameters passed to the real CC  CC实际使用的编译器参数*/
static u32  cc_par_cnt = 1;         /* Param count, including argv0      参数计数*/
static u8   be_quiet,               /* Quiet mode                        */
            clang_mode;             /* Invoked as afl-clang*?            是否使用afl-clang*模式*/


/* Try to find our "fake" GNU assembler in AFL_PATH or at the location derived
   from argv[0]. If that fails, abort. */
//寻找afl-as的位置
static void find_as(u8* argv0) {

  u8 *afl_path = getenv("AFL_PATH");
  u8 *slash, *tmp;
  //首先检查是否存在AFL_PATH这个路径（环境变量），如果存在就赋值给as_path,然后检查afl_path_/as这个文件是否可以访问
  if (afl_path) {

    tmp = alloc_printf("%s/as", afl_path);

    if (!access(tmp, X_OK)) {
      as_path = afl_path;
      ck_free(tmp);
      return;
    }

    ck_free(tmp);

  }
  //如果不存在AFL_PATH这个环境变量，则检查argv0，例如（”/Users/sakura/gitsource/AFL/cmake-build-debug/afl-gcc”）
  //中是否存在’/‘，如果有就找到最后一个’/‘所在的位置，并取其前面的字符串作为dir，然后检查dir/afl-as这个文件是否可以访问，如果可以访问，就将dir设置为as_path
  slash = strrchr(argv0, '/');
  
 
  if (slash) {

    u8 *dir;

    *slash = 0;
    dir = ck_strdup(argv0);
    *slash = '/';

    tmp = alloc_printf("%s/afl-as", dir);

    if (!access(tmp, X_OK)) {
      as_path = dir;
      ck_free(tmp);
      return;
    }

    ck_free(tmp);
    ck_free(dir);

  }

  if (!access(AFL_PATH "/as", X_OK)) {
    as_path = AFL_PATH;
    return;
  }
  //如果以上两种方式都失败，则抛出异常
  FATAL("Unable to find AFL wrapper binary for 'as'. Please set AFL_PATH");
 
}


/* Copy argv to cc_params, making the necessary edits. */
//这个函数主要是将argv拷贝到u8 **cc_params中，并做必要的处理
static void edit_params(u32 argc, char** argv) {

  u8 fortify_set = 0, asan_set = 0;
  u8 *name;

#if defined(__FreeBSD__) && defined(__x86_64__)
  u8 m32_set = 0;
#endif
  //它首先通过ck_alloc来为cc_params分配内存，分配的长度为(argc+128)*8，相当大的内存
  cc_params = ck_alloc((argc + 128) * sizeof(u8*));
  //然后检查argv[0]里有没有’/‘，如果没有就赋值’argv[0]’到name，如果有就找到最后一个’/‘所在的位置，然后跳过这个’/‘，将后面的字符串赋值给name
  name = strrchr(argv[0], '/');
  if (!name) name = argv[0]; else name++;
  //将name与afl-clang比较，如果相同，则设置clang_mode为1，然后设置环境变量CLANG_ENV_VAR为1
  if (!strncmp(name, "afl-clang", 9)) {

    clang_mode = 1;

    setenv(CLANG_ENV_VAR, "1", 1);

    if (!strcmp(name, "afl-clang++")) {   //如果相同，则获取环境变量AFL_CXX的值，如果该值存在，则将cc_params[0]设置为该值，如果不存在，就设置为clang++
      u8* alt_cxx = getenv("AFL_CXX");
      cc_params[0] = alt_cxx ? alt_cxx : (u8*)"clang++";
    } else {                              //如果不相同，则获取环境变量AFL_CC的值，如果该值存在，则将cc_params[0]设置为该值，如果不存在，就设置为clang
      u8* alt_cc = getenv("AFL_CC");
      cc_params[0] = alt_cc ? alt_cc : (u8*)"clang";
    }

  } else {    //如果不相同，则将name和afl-g++比较

    /* With GCJ and Eclipse installed, you can actually compile Java! The
       instrumentation will work (amazingly). Alas, unhandled exceptions do
       not call abort(), so afl-fuzz would need to be modified to equate
       non-zero exit codes with crash conditions when working with Java
       binaries. Meh. */

#ifdef __APPLE__
    
    if (!strcmp(name, "afl-g++")) cc_params[0] = getenv("AFL_CXX");
    else if (!strcmp(name, "afl-gcj")) cc_params[0] = getenv("AFL_GCJ");
    else cc_params[0] = getenv("AFL_CC");

    if (!cc_params[0]) {

      SAYF("\n" cLRD "[-] " cRST
           "On Apple systems, 'gcc' is usually just a wrapper for clang. Please use the\n"
           "    'afl-clang' utility instead of 'afl-gcc'. If you really have GCC installed,\n"
           "    set AFL_CC or AFL_CXX to specify the correct path to that compiler.\n");

      FATAL("AFL_CC or AFL_CXX required on MacOS X");

    }

#else
    //如果相同，则获取环境变量AFL_CXX的值，如果该值存在，则将cc_params[0]设置为该值，如果不存在，就设置为g++
    //如果不相同，则获取环境变量AFL_CC的值，如果该值存在，则将cc_params[0]设置为该值，如果不存在，就设置为gcc
    if (!strcmp(name, "afl-g++")) {
      u8* alt_cxx = getenv("AFL_CXX");
      cc_params[0] = alt_cxx ? alt_cxx : (u8*)"g++";
    } else if (!strcmp(name, "afl-gcj")) {
      u8* alt_cc = getenv("AFL_GCJ");
      cc_params[0] = alt_cc ? alt_cc : (u8*)"gcj";
    } else {  
      u8* alt_cc = getenv("AFL_CC");
      cc_params[0] = alt_cc ? alt_cc : (u8*)"gcc";
    }

#endif /* __APPLE__ */

  }

  //遍历从argv[1]开始的argv参数
  while (--argc) {
    u8* cur = *(++argv);
    //跳过-B/integrated-as/-pipe。
    //-B选项用于设置编译器的搜索路径，直接跳过。（因为在这之前已经处理过as_path了）
    if (!strncmp(cur, "-B", 2)) {

      if (!be_quiet) WARNF("-B is already set, overriding");

      if (!cur[2] && argc > 1) { argc--; argv++; }
      continue;

    }

    if (!strcmp(cur, "-integrated-as")) continue;

    if (!strcmp(cur, "-pipe")) continue;

#if defined(__FreeBSD__) && defined(__x86_64__)
    if (!strcmp(cur, "-m32")) m32_set = 1;
#endif
    //如果存在-fsanitize=address或者-fsanitize=memory，告诉 gcc 检查内存访问的错误，比如数组越界之类，就设置asan_set为1。
    if (!strcmp(cur, "-fsanitize=address") ||
        !strcmp(cur, "-fsanitize=memory")) asan_set = 1;
    //如果存在FORTIFY_SOURCE，则设置fortify_set为1
    //FORTIFY_SOURCE 主要进行缓冲区溢出问题的检查，检查的常见函数有memcpy, mempcpy, memmove, memset, strcpy, stpcpy, strncpy, strcat, strncat, sprintf, vsprintf, snprintf, gets 等
    if (strstr(cur, "FORTIFY_SOURCE")) fortify_set = 1;
    //对 cc_params 进行赋值
    cc_params[cc_par_cnt++] = cur;

  }

  //开始设置其他的cc_params参数

  //取之前计算出来的as_path，然后设置-B as_path
  cc_params[cc_par_cnt++] = "-B";
  cc_params[cc_par_cnt++] = as_path;

  if (clang_mode)
    cc_params[cc_par_cnt++] = "-no-integrated-as";

  if (getenv("AFL_HARDEN")) {

    cc_params[cc_par_cnt++] = "-fstack-protector-all";

    if (!fortify_set)
      cc_params[cc_par_cnt++] = "-D_FORTIFY_SOURCE=2";

  }

  //sanitizer
  //如果asan_set在上面被设置为1，则使AFL_USE_ASAN环境变量为1
  if (asan_set) {

    /* Pass this on to afl-as to adjust map density. */
    
    setenv("AFL_USE_ASAN", "1", 1);

  } else if (getenv("AFL_USE_ASAN")) {  //如果 asan_set 不为1且，存在 AFL_USE_ASAN 环境变量，则设置-U_FORTIFY_SOURCE -fsanitize=address

    if (getenv("AFL_USE_MSAN"))   
      FATAL("ASAN and MSAN are mutually exclusive");

    if (getenv("AFL_HARDEN"))
      FATAL("ASAN and AFL_HARDEN are mutually exclusive");

    cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
    cc_params[cc_par_cnt++] = "-fsanitize=address";

  } else if (getenv("AFL_USE_MSAN")) {    //如果存在AFL_USE_MSAN环境变量，则设置-fsanitize=memory，但不能同时还指定AFL_HARDEN或者AFL_USE_ASAN，因为这样运行时速度过慢

    if (getenv("AFL_USE_ASAN"))
      FATAL("ASAN and MSAN are mutually exclusive");

    if (getenv("AFL_HARDEN"))
      FATAL("MSAN and AFL_HARDEN are mutually exclusive");

    cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
    cc_params[cc_par_cnt++] = "-fsanitize=memory";


  }

  if (!getenv("AFL_DONT_OPTIMIZE")) {

#if defined(__FreeBSD__) && defined(__x86_64__)

    /* On 64-bit FreeBSD systems, clang -g -m32 is broken, but -m32 itself
       works OK. This has nothing to do with us, but let's avoid triggering
       that bug. */

    if (!clang_mode || !m32_set)
      cc_params[cc_par_cnt++] = "-g";

#else

      cc_params[cc_par_cnt++] = "-g";

#endif

    cc_params[cc_par_cnt++] = "-O3";
    cc_params[cc_par_cnt++] = "-funroll-loops";

    /* Two indicators that you're building for fuzzing; one of them is
       AFL-specific, the other is shared with libfuzzer. */

    cc_params[cc_par_cnt++] = "-D__AFL_COMPILER=1";
    cc_params[cc_par_cnt++] = "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1";

  }
  //如果存在 AFL_NO_BUILTIN 环境变量，则表示允许进行优化
  if (getenv("AFL_NO_BUILTIN")) {

    cc_params[cc_par_cnt++] = "-fno-builtin-strcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-memcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strstr";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasestr";

  }
  
  //终止对cc_params的编辑
  cc_params[cc_par_cnt] = NULL;

}


/* Main entry point */
//afl-gcc就是找到as所在的位置，将其加入搜索路径，然后设置必要的gcc参数和一些宏，然后调用gcc进行实际的编译，仅仅只是一层wrapper

int main(int argc, char** argv) {

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-cc " cBRI VERSION cRST " by <lcamtuf@google.com>\n");

  } else be_quiet = 1;

  if (argc < 2) {

    SAYF("\n"
         "This is a helper application for afl-fuzz. It serves as a drop-in replacement\n"
         "for gcc or clang, letting you recompile third-party code with the required\n"
         "runtime instrumentation. A common use pattern would be one of the following:\n\n"

         "  CC=%s/afl-gcc ./configure\n"
         "  CXX=%s/afl-g++ ./configure\n\n"

         "You can specify custom next-stage toolchain via AFL_CC, AFL_CXX, and AFL_AS.\n"
         "Setting AFL_HARDEN enables hardening optimizations in the compiled code.\n\n",
         BIN_PATH, BIN_PATH);

    exit(1);

  }
  //查找fake GNU assembler
  find_as(argv[0]);
  // 设置CC的参数；处理传入的编译参数，将确定好的参数放入 cc_params[] 数组
  edit_params(argc, argv);
  // 调用execvp来执行CC
/* 测试代码，打印参数
  printf("---------------------------------\n");
  for(int i = 0; i < sizeof(cc_params); i++) {
    printf("\tag:%d: %s\n", i, cc_params[i]);
  }
  printf("---------------------------------\n");
*/
  execvp(cc_params[0], (char**)cc_params);

  FATAL("Oops, failed to execute '%s' - check your PATH", cc_params[0]);

  return 0;

}
