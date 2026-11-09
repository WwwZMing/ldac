/* MSVC 没有 <unistd.h>。libldac 只是把它当"标准头集合"之一包含进来，
 * 实际并不调用 POSIX API，所以给一个转发到 MSVC 等价头即可。 */
#ifndef LDAC_MSVC_UNISTD_H
#define LDAC_MSVC_UNISTD_H

#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <process.h>

#endif
