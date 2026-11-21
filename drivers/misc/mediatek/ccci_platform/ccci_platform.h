/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CCCI_PLATFORM_H__
#define __CCCI_PLATFORM_H__

struct regmap;

/* The MT6358 regmap (over pwrap) U's mfd made, or NULL before it binds. Accesses can sleep. */
struct regmap *ccci_pmic_regmap(void);

#endif
