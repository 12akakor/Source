/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

// A library whose TLS block is created and destroyed across dlopen/dlclose.
__thread int sTlsValue = 0;

extern "C" int
set_tls_value(int value)
{
	sTlsValue = value;
	return sTlsValue;
}

extern "C" int
get_tls_value()
{
	return sTlsValue;
}
