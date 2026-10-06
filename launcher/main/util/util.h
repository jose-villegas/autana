/*
 * util: code every layer shares, in two kinds. runtime/ holds the services
 * over the chip (time, memory, settings, jobs, frame cost and watch,
 * tunables, the build id); math/, scalar/, motion/ and encode/ hold pure
 * code that runs the same on a host and includes nothing from runtime/.
 *
 * This header holds the folder's description only; include a subfolder's
 * header for the code.
 */
#pragma once
