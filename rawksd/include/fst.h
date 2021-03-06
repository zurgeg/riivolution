/*

libfst -- a Wii disc filesystem devoptab library for the Wii

Copyright (C) 2008 Joseph Jordan <joe.ftpii@psychlaw.com.au>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1.The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software in a
product, an acknowledgment in the product documentation would be
appreciated but is not required.

2.Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3.This notice may not be removed or altered from any source distribution.

*/
#ifndef _FST_H
#define _FST_H

#define FST_MAXPATHLEN 128
#define ISO_MAXPATHLEN 128


#ifdef __cplusplus
extern "C" {
#endif

u64 FST_Mount();
bool FST_Unmount();
u64 FST_LastAccess();

bool ISO9660_Mount();
bool ISO9660_Unmount();
u64 ISO9660_LastAccess();

#ifdef __cplusplus
}
#endif

#endif /* _FST_H_ */
