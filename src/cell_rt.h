/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2022 Mladen Ivkovic (mladen.ivkovic@hotmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#ifndef SWIFT_CELL_RT_H
#define SWIFT_CELL_RT_H

/* Config parameters. */
/* #include "../config.h" */

/* Local includes. */
/* #include "lock.h" */
#include "timeline.h"

/**
 * @brief Radiative transfer related cell variables.
 */
struct cell_rt {

  /* If we are not using RT, compact as much of the unecessary variables
     into an anonymous union to save memory in the cell structure. */
#ifdef RT_NONE
  union {
#endif

    /*! Radiative transfer ghost in task */
    struct task *rt_in;

    /*! Radiative transfer ghost1 task (finishes up injection) */
    struct task *rt_ghost1;

    /*! Task for self/pair gradient step of radiative transfer */
    struct link *rt_gradient;

    /*! Radiative transfer ghost2 task */
    struct task *rt_ghost2;

    /*! Task for self/pair transport step of radiative transfer */
    struct link *rt_transport;

    /*! Radiative transfer transport out task */
    struct task *rt_transport_out;

    /*! Radiative transfer thermochemistry task */
    struct task *rt_tchem;

    /*! Radiative transfer cell time advancement task */
    struct task *rt_advance_cell_time;

    /*! Radiative transfer ghost out task */
    struct task *rt_out;

#ifdef SWIFT_DEBUG_CHECKS

    /*! Last (integer) time the cell's sort arrays were updated. */
    integertime_t ti_sort;

#endif

#ifdef RT_NONE
  };
#endif

  /*! Minimum end of (integer) time step in this cell for RT tasks. */
  integertime_t ti_rt_end_min;

  /*! Maximum beginning of (integer) time step in this cell for RT tasks. */
  integertime_t ti_rt_beg_max;

  /*! Minimum (integer) time step size in this cell for RT tasks. */
  integertime_t ti_rt_min_step_size;
};

#endif /* SWIFT_CELL_RT_H */
