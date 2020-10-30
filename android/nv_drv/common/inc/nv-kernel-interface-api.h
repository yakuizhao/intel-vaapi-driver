/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2018 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#ifndef _NV_KERNEL_INTERFACE_API_H
#define _NV_KERNEL_INTERFACE_API_H
/**************************************************************************************************************
*
*    File:  nv-kernel-interface-api.h
*
*    Description:
*        Defines the NV API related macros. 
*
**************************************************************************************************************/

#if (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) >= 40600
#define __ALTSTACK_ATTRIBUTE_ARG__ 0
#else
#define __ALTSTACK_ATTRIBUTE_ARG__ false
#endif

#if defined(NV_UNIX)
#if defined(NVCPU_X86)
#if defined(__use_altstack__)
#define NV_API_CALL __attribute__((regparm(0),altstack(__ALTSTACK_ATTRIBUTE_ARG__)))
#else
#define NV_API_CALL __attribute__((regparm(0)))
#endif
#elif defined(NVCPU_X86_64) && defined(__use_altstack__)
#define NV_API_CALL __attribute__((altstack(__ALTSTACK_ATTRIBUTE_ARG__)))
#else
#define NV_API_CALL
#endif /* defined(NVCPU_X86) */
#else
#define NV_API_CALL
#endif /* defined(NV_UNIX) */

#endif /*  _NV_API_H */
