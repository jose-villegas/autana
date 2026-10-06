/*
 * util: code every layer shares, in two kinds. runtime/ holds the services
 * that reach the chip; math/, scalar/, motion/, encode/ and build/ never
 * touch the chip and include nothing from runtime/.
 *
 * This header holds the folder's description only; include a subfolder's
 * header for the code.
 */
#pragma once
