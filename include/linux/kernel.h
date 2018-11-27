#ifndef COMPAT_KERNEL_H
#define COMPAT_KERNEL_H

#include_next <linux/kernel.h>

#ifndef DIV_ROUND_UP_ULL
#define DIV_ROUND_UP_ULL(ll,d) \
	({ unsigned long long _tmp = (ll)+(d)-1; do_div(_tmp, d); _tmp; })
#endif

#endif /* COMPAT_KERNEL_H */
