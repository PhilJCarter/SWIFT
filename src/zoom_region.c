/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2012 Pedro Gonnet (pedro.gonnet@durham.ac.uk)
 *                    Matthieu Schaller (matthieu.schaller@durham.ac.uk)
 *               2015 Peter W. Draper (p.w.draper@durham.ac.uk)
 *               2016 John A. Regan (john.a.regan@durham.ac.uk)
 *                    Tom Theuns (tom.theuns@durham.ac.uk)
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

/* Config parameters. */
#include "zoom_region.h"

#include "../config.h"
#include "cell.h"
#include "engine.h"
#include "gravity_properties.h"
#include "proxy.h"
#include "space.h"

#include <float.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

/**
 * @brief Read parameter file for "ZoomRegion" properties, and initialize the
 * zoom_region struct.
 *
 * @param params Swift parameter structure.
 * @param s The space
 */
void zoom_region_init(struct swift_params *params, struct space *s,
                      int verbose) {
#ifdef WITH_ZOOM_REGION
  /* Are we running with a zoom region? */
  s->with_zoom_region =
      parser_get_opt_param_int(params, "ZoomRegion:enable", 0);

  /* If so... */
  if (s->with_zoom_region) {

    /* Zoom region properties are stored in a structure. */
    s->zoom_props = (struct zoom_region_properties *)malloc(
        sizeof(struct zoom_region_properties));
    if (s->zoom_props == NULL)
      error("Error allocating memory for the zoom parameters.");

    /* Are refining the background cells? */
    s->zoom_props->refine_bkg =
        parser_get_opt_param_int(params, "ZoomRegion:enable_bkg_refinement", 1);

    /* Set the zoom cdim. */
    s->zoom_props->cdim[0] =
        parser_get_opt_param_int(params, "Scheduler:max_top_level_cells",
                                 space_max_top_level_cells_default);
    s->zoom_props->cdim[1] =
        parser_get_opt_param_int(params, "Scheduler:max_top_level_cells",
                                 space_max_top_level_cells_default);
    s->zoom_props->cdim[2] =
        parser_get_opt_param_int(params, "Scheduler:max_top_level_cells",
                                 space_max_top_level_cells_default);

    /* Extract the zoom width boost factor (used to define the buffer around the
     * zoom region). */
    s->zoom_props->zoom_boost_factor =
        parser_get_opt_param_float(params, "ZoomRegion:zoom_boost_factor", 1.1);

    /* Set the number of zoom cells in a natural cell. */
    s->zoom_props->nr_zoom_per_bkg_cells = s->zoom_props->cdim[0];

    /* Initialise the number of wanders (unused if with_hydro == False)*/
    s->zoom_props->nr_wanderers = 0;

    /* Get an initial dimension for the zoom region and its geometric mid point.
     */
    double new_zoom_boundary[6] = {1e20, -1e20, 1e20, -1e20, 1e20, -1e20};
    double midpoint[3] = {0.0, 0.0, 0.0};
    const size_t nr_gparts = s->nr_gparts;
    double mtot = 0.0;
    double com[3] = {0.0, 0.0, 0.0};

    /* Get the shift from the ICs since this hasn't been applied yet. */
    double shift[3] = {0.0, 0.0, 0.0};
    parser_get_opt_param_double_array(params, "InitialConditions:shift", 3,
                                      shift);

    /* Find the min/max location in each dimension for each mask gravity
     * particle, and their COM. */
    for (size_t k = 0; k < nr_gparts; k++) {
      if (s->gparts[k].type != swift_type_dark_matter) continue;

      /* Shift initial positions by IC shift. */
      const double x = s->gparts[k].x[0] + shift[0];
      const double y = s->gparts[k].x[1] + shift[1];
      const double z = s->gparts[k].x[2] + shift[2];

      /* Wrap if periodic. */
      if (s->periodic) {
        box_wrap(x, 0.0, s->dim[0]);
        box_wrap(y, 0.0, s->dim[1]);
        box_wrap(z, 0.0, s->dim[2]);
      }

      /* Ammend boundaries for this particle. */
      if (x < new_zoom_boundary[0]) new_zoom_boundary[0] = x;
      if (x > new_zoom_boundary[1]) new_zoom_boundary[1] = x;
      if (y < new_zoom_boundary[2]) new_zoom_boundary[2] = y;
      if (y > new_zoom_boundary[3]) new_zoom_boundary[3] = y;
      if (z < new_zoom_boundary[4]) new_zoom_boundary[4] = z;
      if (z > new_zoom_boundary[5]) new_zoom_boundary[5] = z;

      /* Total up mass and position for COM. */
      mtot += s->gparts[k].mass;
      com[0] += x * s->gparts[k].mass;
      com[1] += y * s->gparts[k].mass;
      com[2] += z * s->gparts[k].mass;
    }

#ifdef WITH_MPI
    /* Share answers amoungst nodes. */

    /* Boundary. */
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[0], 1, MPI_DOUBLE, MPI_MIN,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[1], 1, MPI_DOUBLE, MPI_MAX,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[2], 1, MPI_DOUBLE, MPI_MIN,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[3], 1, MPI_DOUBLE, MPI_MAX,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[4], 1, MPI_DOUBLE, MPI_MIN,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &new_zoom_boundary[5], 1, MPI_DOUBLE, MPI_MAX,
                  MPI_COMM_WORLD);

    /* CoM. */
    MPI_Allreduce(MPI_IN_PLACE, com, 3, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &mtot, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
#endif

    /* Finalize CoM calcuation. */
    const double imass = 1.0 / mtot;
    com[0] *= imass;
    com[1] *= imass;
    com[2] *= imass;

    /* Store result. */
    for (int ijk = 0; ijk < 3; ijk++) s->zoom_props->com[ijk] = com[ijk];

    if (verbose) message("com: [%f %f %f]", com[0], com[1], com[2]);

    if (verbose)
      message(
          "initial_dim: [%f %f %f] initial_zoom_boundary: [%f-%f %f-%f %f-%f]",
          new_zoom_boundary[1] - new_zoom_boundary[0],
          new_zoom_boundary[3] - new_zoom_boundary[2],
          new_zoom_boundary[5] - new_zoom_boundary[4], new_zoom_boundary[0],
          new_zoom_boundary[1], new_zoom_boundary[2], new_zoom_boundary[3],
          new_zoom_boundary[4], new_zoom_boundary[5]);

    /* Get the initial dimensions and midpoint. */
    double ini_dim[3] = {0.0, 0.0, 0.0};
    for (int ijk = 0; ijk < 3; ijk++) {
      ini_dim[ijk] =
          (new_zoom_boundary[(ijk * 2) + 1] - new_zoom_boundary[ijk * 2]);
      midpoint[ijk] = new_zoom_boundary[(ijk * 2) + 1] - (ini_dim[ijk] / 2);
    }

    /* Throw an error if the zoom region extends over the box boundries.
     * TODO: This could be fixed automatically! */
    double shiftx = 0.;
    double shifty = 0.;
    double shiftz = 0.;
    if ((ini_dim[0] > s->dim[0] / 2) || (ini_dim[1] > s->dim[1] / 2) ||
        (ini_dim[2] > s->dim[2] / 2)) {
      if (ini_dim[0] > s->dim[0] / 2) shiftx = s->dim[0] / 2;
      if (ini_dim[1] > s->dim[1] / 2) shifty = s->dim[1] / 2;
      if (ini_dim[2] > s->dim[2] / 2) shiftz = s->dim[2] / 2;
      error(
          "Zoom region extends beyond the boundaries of the box. "
          "Shift the ICs by [%f, %f, %f]",
          shiftx, shifty, shiftz);
    }

    /* Calculate the shift needed to place the mid point of the high res
     * particles at the centre of the box. This shift is applied to the
     * particles in space_init in space.c */
    const double box_mid[3] = {s->dim[0] / 2, s->dim[1] / 2, s->dim[2] / 2};
    for (int ijk = 0; ijk < 3; ijk++) {
      s->zoom_props->zoom_shift[ijk] = box_mid[ijk] - midpoint[ijk];
    }
    if (verbose) {
      message("box_mid = [%f %f %f] midpoint = [%f %f %f]", box_mid[0],
              box_mid[1], box_mid[2], midpoint[0], midpoint[1], midpoint[2]);
      message("Need to shift the box by [%e, %e, %e] to centre the zoom region",
              s->zoom_props->zoom_shift[0], s->zoom_props->zoom_shift[1],
              s->zoom_props->zoom_shift[2]);
    }

    /* Let's shift the COM.
     * NOTE: boundaries are recalculated relative to box centre later. */
    for (int ijk = 0; ijk < 3; ijk++)
      s->zoom_props->com[ijk] += s->zoom_props->zoom_shift[ijk];

    /* Compute maximum side length of the zoom region, we need zoom dim to be
     * equal. */
    double max_dim = max3(ini_dim[0], ini_dim[1], ini_dim[2]) *
                     s->zoom_props->zoom_boost_factor;

    /* This width has to divide the full parent box by an odd integer to ensure
     * the two grids line up. NOTE: assumes box dimensions are equal! */
    int nr_zoom_regions = (int)(s->dim[0] / max_dim);
    if (nr_zoom_regions % 2 == 0) nr_zoom_regions -= 1;
    max_dim = s->dim[0] / nr_zoom_regions;

    /* Do we want to refine the background cells? */
    if (s->zoom_props->refine_bkg &&
        nr_zoom_regions >= s->zoom_props->cdim[0]) {

      /* Start with max_top_level_cells as a guess. */
      nr_zoom_regions = s->zoom_props->cdim[0];

      /* Must be odd. */
      if (nr_zoom_regions % 2 == 0) nr_zoom_regions -= 1;

      /* Compute the new boost factor and store it for reporting. */
      const float new_zoom_boost_factor =
          (s->dim[0] / nr_zoom_regions) /
          (max_dim / s->zoom_props->zoom_boost_factor);

      if (verbose)
        message("Have increased zoom_boost_factor from %f to %f",
                s->zoom_props->zoom_boost_factor, new_zoom_boost_factor);

      /* Assign the new values. */
      max_dim = s->dim[0] / nr_zoom_regions;
      s->zoom_props->zoom_boost_factor = new_zoom_boost_factor;
    }

    /* Find the new boundaries with this extra width and boost factor.
     * The zoom region is already centred on the middle of the box */
    for (int ijk = 0; ijk < 3; ijk++) {
      /* Set the new boundaries. */
      s->zoom_props->region_bounds[(ijk * 2)] =
          (s->dim[ijk] / 2) - (max_dim / 2);
      s->zoom_props->region_bounds[(ijk * 2) + 1] =
          (s->dim[ijk] / 2) + (max_dim / 2);

      /* Set the reigon dim. */
      s->zoom_props->dim[ijk] = max_dim;
    }

    /* Set the minimum allowed zoom cell width. */
    const double zoom_dmax = max3(s->zoom_props->dim[0], s->zoom_props->dim[1],
                                  s->zoom_props->dim[2]);
    s->zoom_props->cell_min = 0.99 * zoom_dmax / s->zoom_props->cdim[0];
  }
#endif
}

/**
 * @brief For a given particle location, what TL cell does it belong to?
 *
 * Slightly more complicated in the zoom case, as there are now two embedded TL
 * grids.
 *
 * First see if the particle is within the zoom bounds, then find its TL cell.
 *
 * @param s The space.
 * @param x, y, z Location of particle.
 */
int cell_getid_zoom(const struct space *s, const double x, const double y,
                    const double z) {
#ifdef WITH_ZOOM_REGION
  int cell_id;

  /* Lets get some space information */
  const int cdim[3] = {s->cdim[0], s->cdim[1], s->cdim[2]};
  const double iwidth[3] = {s->iwidth[0], s->iwidth[1], s->iwidth[2]};

  /* Lets get some properties of the zoom region. */
  const struct zoom_region_properties *zoom_props = s->zoom_props;
  const int zoom_cdim[3] = {zoom_props->cdim[0], zoom_props->cdim[1],
                            zoom_props->cdim[2]};
  const double zoom_iwidth[3] = {zoom_props->iwidth[0], zoom_props->iwidth[1],
                                 zoom_props->iwidth[2]};
  const int bkg_cell_offset = zoom_props->tl_cell_offset;
  const double zoom_region_bounds[6] = {
      zoom_props->region_bounds[0], zoom_props->region_bounds[1],
      zoom_props->region_bounds[2], zoom_props->region_bounds[3],
      zoom_props->region_bounds[4], zoom_props->region_bounds[5]};

  /* Get the background cell ijk coordinates. */
  const int bkg_i = x * iwidth[0];
  const int bkg_j = y * iwidth[1];
  const int bkg_k = z * iwidth[2];

  /* Are the passed coordinates within the zoom region? */
  if (bkg_i == s->zoom_props->zoom_cell_ijk[0] &&
      bkg_j == s->zoom_props->zoom_cell_ijk[1] &&
      bkg_k == s->zoom_props->zoom_cell_ijk[2]) {

    /* Which zoom TL cell are we in? */
    const int zoom_i = (x - zoom_region_bounds[0]) * zoom_iwidth[0];
    const int zoom_j = (y - zoom_region_bounds[2]) * zoom_iwidth[1];
    const int zoom_k = (z - zoom_region_bounds[4]) * zoom_iwidth[2];
    cell_id = cell_getid(zoom_cdim, zoom_i, zoom_j, zoom_k);

#ifdef SWIFT_DEBUG_CHECKS
    if (cell_id < 0 || cell_id >= zoom_cdim[0] * zoom_cdim[1] * zoom_cdim[2])
      error("cell_id out of range: %i (%f %f %f)", cell_id, x, y, z);
#endif

    /* If not then treat it like normal, and find the natural TL cell. */
  } else {
    cell_id = cell_getid(cdim, bkg_i, bkg_j, bkg_k) + bkg_cell_offset;

#ifdef SWIFT_DEBUG_CHECKS
    if (cell_id < bkg_cell_offset || cell_id >= s->nr_cells)
      error("cell_id out of range: %i (%f %f %f)", cell_id, x, y, z);
#endif
  }

  return cell_id;
#else
  error("Using cell_getid_zoom but compiled without zoom regions enabled!");
#endif
}

#ifdef WITH_ZOOM_REGION
#ifdef SWIFT_DEBUG_CHECKS
/**
 * @brief Run through all cells and ensure they have the correct cell type and
 * width for their position in s->cells_top.
 *
 * @param s The space.
 */
static void debug_cell_type(struct space *s) {

  /* Get the cells array and cell properties */
  struct cell *cells = s->cells_top;
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;
  const double *zoom_width = s->zoom_props->width;
  const double *width = s->width;

  /* Loop over all cells */
  for (int cid = 0; cid < s->nr_cells; cid++) {

    /* Check cell type */
    if (cid < bkg_cell_offset && cells[cid].tl_cell_type != 3)
      error(
          "Cell has the wrong cell type for it's array position (cid=%d, "
          "c->tl_cell_type=%d, "
          "s->zoom_props->tl_cell_offset=%d)",
          cid, cells[cid].tl_cell_type, bkg_cell_offset);
    if (cid >= bkg_cell_offset && cells[cid].tl_cell_type == 3)
      error(
          "Cell has the wrong cell type for it's array position (cid=%d, "
          "c->tl_cell_type=%d, "
          "s->zoom_props->tl_cell_offset=%d)",
          cid, cells[cid].tl_cell_type, bkg_cell_offset);

    /* Check cell widths */
    for (int ijk = 0; ijk < 3; ijk++) {
      if (cid < bkg_cell_offset && cells[cid].width[ijk] != zoom_width[ijk])
        error(
            "Cell has the wrong cell width for it's array position (cid=%d, "
            "c->tl_cell_type=%d, "
            "s->zoom_props->tl_cell_offset=%d, c->width=[%f %f %f], "
            "s->zoom_props->width=[%f %f %f])",
            cid, cells[cid].tl_cell_type, bkg_cell_offset, cells[cid].width[0],
            cells[cid].width[1], cells[cid].width[2], s->zoom_props->width[0],
            s->zoom_props->width[1], s->zoom_props->width[2]);
      if (cid >= bkg_cell_offset && cells[cid].width[ijk] != width[ijk])
        error(
            "Cell has the wrong cell width for it's array position (cid=%d, "
            "c->tl_cell_type=%d, "
            "s->zoom_props->tl_cell_offset=%d, c->width=[%f %f %f], "
            "s->zoom_props->width=[%f %f %f])",
            cid, cells[cid].tl_cell_type, bkg_cell_offset, cells[cid].width[0],
            cells[cid].width[1], cells[cid].width[2], s->zoom_props->width[0],
            s->zoom_props->width[1], s->zoom_props->width[2]);
    }
  }
}
#endif
#endif

/**
 * @brief Compute the extent/bounds of the zoom region using the high-res DM
 * particles.
 *
 * The min/max [x,y,z] for each particle is found, and the CoM of these
 * particles is computed.
 *
 * @param s The space.
 * @param verbose Are we talking?
 */
void construct_zoom_region(struct space *s, int verbose) {
#ifdef WITH_ZOOM_REGION
  /* Get the width of the zoom region, zoom dims are equal. */
  const double zoom_dim = s->zoom_props->dim[0];

  /* Let's set what we know about the zoom region. */
  for (int ijk = 0; ijk < 3; ijk++) {
    s->zoom_props->width[ijk] = zoom_dim / s->zoom_props->cdim[ijk];
    s->zoom_props->iwidth[ijk] = 1 / s->zoom_props->width[ijk];
    s->zoom_props->dim[ijk] = zoom_dim;
  }

  /* Overwrite the minimum allowed zoom cell width. */
  s->zoom_props->cell_min = 0.99 * zoom_dim / s->zoom_props->cdim[0];

  /* Now we can define the background grid and zoom region's background ijk. */
  for (int ijk = 0; ijk < 3; ijk++) {
    s->width[ijk] = s->zoom_props->dim[ijk];
    s->iwidth[ijk] = 1.0 / s->width[ijk];
    s->cdim[ijk] =
        (int)floor((s->dim[ijk] + 0.1 * s->width[ijk]) * s->iwidth[ijk]);
    s->zoom_props->zoom_cell_ijk[ijk] = (int)floor(s->cdim[ijk] / 2);
  }

  /* Resize the top level cells in the space. */
  const double dmax = max3(s->dim[0], s->dim[1], s->dim[2]);
  const int nr_zoom_regions = dmax / zoom_dim;
  s->cell_min = 0.99 * dmax / nr_zoom_regions;

  /* Check we have enough cells for periodicity. */
  if (s->periodic && (s->cdim[0] < 3 || s->cdim[1] < 3 || s->cdim[2] < 3))
    error(
        "Must have at least 3 cells in each spatial dimension when periodicity "
        "is switched on.\nThis error is often caused by any of the "
        "followings:\n"
        " - too few particles to generate a sensible grid,\n"
        " - the initial value of 'Scheduler:max_top_level_cells' is too "
        "small,\n"
        " - the (minimal) time-step is too large leading to particles with "
        "predicted smoothing lengths too large for the box size,\n"
        " - particles with velocities so large that they move by more than two "
        "box sizes per time-step.\n");

  /* Store cell number information. */
  s->zoom_props->tl_cell_offset =
      s->zoom_props->cdim[0] * s->zoom_props->cdim[1] * s->zoom_props->cdim[2];
  s->zoom_props->nr_zoom_cells =
      s->zoom_props->cdim[0] * s->zoom_props->cdim[1] * s->zoom_props->cdim[2];
  s->zoom_props->nr_bkg_cells = s->cdim[0] * s->cdim[1] * s->cdim[2];

  /* Get the cell index for the parent background cell */
  s->zoom_props->void_cell_index =
      cell_getid(s->cdim, s->zoom_props->zoom_cell_ijk[0],
                 s->zoom_props->zoom_cell_ijk[1],
                 s->zoom_props->zoom_cell_ijk[2]) +
      s->zoom_props->tl_cell_offset;

  /* Lets report what we have constructed. */
  if (verbose) {
    message(
        "set cell dimensions to zoom_cdim=[%d %d %d] background_cdim=[%d %d "
        "%d]",
        s->zoom_props->cdim[0], s->zoom_props->cdim[1], s->zoom_props->cdim[2],
        s->cdim[0], s->cdim[1], s->cdim[2]);
    message("zoom region located in cell [%d %d %d]",
            s->zoom_props->zoom_cell_ijk[0], s->zoom_props->zoom_cell_ijk[1],
            s->zoom_props->zoom_cell_ijk[2]);
    message("nr_zoom_cells: %d nr_bkg_cells: %d tl_cell_offset: %d",
            s->zoom_props->nr_zoom_cells, s->zoom_props->nr_bkg_cells,
            s->zoom_props->tl_cell_offset);
    message("zoom_boundary: [%f-%f %f-%f %f-%f]",
            s->zoom_props->region_bounds[0], s->zoom_props->region_bounds[1],
            s->zoom_props->region_bounds[2], s->zoom_props->region_bounds[3],
            s->zoom_props->region_bounds[4], s->zoom_props->region_bounds[5]);
    message(
        "dim: [%f %f %f] tl_cell_width: [%f %f %f] zoom_cell_width: [%f %f %f]",
        s->zoom_props->dim[0], s->zoom_props->dim[1], s->zoom_props->dim[2],
        s->width[0], s->width[1], s->width[2], s->zoom_props->width[0],
        s->zoom_props->width[1], s->zoom_props->width[2]);
    message(
        "nr_tl_cells_in_zoom_region: [%d %d %d] nr_zoom_cells_in_tl_cell: [%d "
        "%d %d]",
        (int)floor((s->zoom_props->dim[0] + 0.5 * s->width[0]) * s->iwidth[0]),
        (int)floor((s->zoom_props->dim[1] + 0.5 * s->width[1]) * s->iwidth[1]),
        (int)floor((s->zoom_props->dim[2] + 0.5 * s->width[2]) * s->iwidth[2]),
        (int)floor((s->width[0] + 0.5 * s->zoom_props->width[0]) *
                   s->zoom_props->iwidth[0]),
        (int)floor((s->width[1] + 0.5 * s->zoom_props->width[1]) *
                   s->zoom_props->iwidth[1]),
        (int)floor((s->width[2] + 0.5 * s->zoom_props->width[2]) *
                   s->zoom_props->iwidth[2]));
  }

#endif
}

/**
 * @brief Build the TL cells, with a zoom region.
 *
 * This replaces the loop in space_regrid when running with a zoom region.
 *
 * Construct an additional set of TL "zoom" cells embedded within the TL cell
 * structure with the dimensions of each cell structure being the same (with
 * differing widths).
 *
 * Therefore the new TL cell structure is 2*cdim**3, with the "natural" TL cells
 * occupying the first half of the TL cell list, and the "zoom" TL cells
 * ocupying the second half.
 *
 * @param s The space.
 * @param verbose Are we talking?
 */
void construct_tl_cells_with_zoom_region(
    struct space *s, const int *cdim, const float dmin,
    const integertime_t ti_current, struct gravity_props *gravity_properties,
    int verbose) {
#ifdef WITH_ZOOM_REGION

  /* Get some zoom region properties */
  const float dmin_zoom = min3(s->zoom_props->width[0], s->zoom_props->width[1],
                               s->zoom_props->width[2]);
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;
  const double zoom_region_bounds[6] = {
      s->zoom_props->region_bounds[0], s->zoom_props->region_bounds[1],
      s->zoom_props->region_bounds[2], s->zoom_props->region_bounds[3],
      s->zoom_props->region_bounds[4], s->zoom_props->region_bounds[5]};

  struct cell *restrict c;

  /* Loop over zoom cells and set locations and initial values */
  for (int i = 0; i < s->zoom_props->cdim[0]; i++) {
    for (int j = 0; j < s->zoom_props->cdim[1]; j++) {
      for (int k = 0; k < s->zoom_props->cdim[2]; k++) {
        const size_t cid = cell_getid(s->zoom_props->cdim, i, j, k);

        /* Create the zoom cell and it's multipoles */
        c = &s->cells_top[cid];
        c->loc[0] = i * s->zoom_props->width[0] + zoom_region_bounds[0];
        c->loc[1] = j * s->zoom_props->width[1] + zoom_region_bounds[2];
        c->loc[2] = k * s->zoom_props->width[2] + zoom_region_bounds[4];
        c->parent_bkg_cid = s->zoom_props->void_cell_index;
        c->width[0] = s->zoom_props->width[0];
        c->width[1] = s->zoom_props->width[1];
        c->width[2] = s->zoom_props->width[2];
        if (s->with_self_gravity) c->grav.multipole = &s->multipoles_top[cid];
        c->tl_cell_type = zoom_tl_cell;
        c->dmin = dmin_zoom;
        c->nr_zoom_per_bkg_cells = s->zoom_props->nr_zoom_per_bkg_cells;
        c->depth = 0;
        c->split = 0;
        c->hydro.count = 0;
        c->grav.count = 0;
        c->stars.count = 0;
        c->sinks.count = 0;
        c->top = c;
        c->super = c;
        c->hydro.super = c;
        c->grav.super = c;
        c->hydro.ti_old_part = ti_current;
        c->grav.ti_old_part = ti_current;
        c->stars.ti_old_part = ti_current;
        c->sinks.ti_old_part = ti_current;
        c->black_holes.ti_old_part = ti_current;
        c->grav.ti_old_multipole = ti_current;
#ifdef WITH_MPI
        c->mpi.tag = -1;
        c->mpi.recv = NULL;
        c->mpi.send = NULL;
#endif
#if defined(SWIFT_DEBUG_CHECKS) || defined(SWIFT_CELL_GRAPH)
        cell_assign_top_level_cell_index(c, s);
#endif
      }
    }
  }
  /* Loop over natural cells and set locations and initial values */
  for (int i = 0; i < cdim[0]; i++) {
    for (int j = 0; j < cdim[1]; j++) {
      for (int k = 0; k < cdim[2]; k++) {
        const size_t cid = cell_getid(cdim, i, j, k);

        /* Natural top level cells. */
        c = &s->cells_top[cid + bkg_cell_offset];
        c->loc[0] = i * s->width[0];
        c->loc[1] = j * s->width[1];
        c->loc[2] = k * s->width[2];
        c->width[0] = s->width[0];
        c->width[1] = s->width[1];
        c->width[2] = s->width[2];
        c->dmin = dmin;
        c->parent_bkg_cid = cid + bkg_cell_offset;
        c->nr_zoom_per_bkg_cells = s->zoom_props->nr_zoom_per_bkg_cells;
        if (s->with_self_gravity)
          c->grav.multipole = &s->multipoles_top[cid + bkg_cell_offset];
        c->tl_cell_type = tl_cell;
        c->depth = 0;
        c->split = 0;
        c->hydro.count = 0;
        c->grav.count = 0;
        c->stars.count = 0;
        c->sinks.count = 0;
        c->top = c;
        c->super = c;
        c->hydro.super = c;
        c->grav.super = c;
        c->hydro.ti_old_part = ti_current;
        c->grav.ti_old_part = ti_current;
        c->stars.ti_old_part = ti_current;
        c->sinks.ti_old_part = ti_current;
        c->black_holes.ti_old_part = ti_current;
        c->grav.ti_old_multipole = ti_current;
#ifdef WITH_MPI
        c->mpi.tag = -1;
        c->mpi.recv = NULL;
        c->mpi.send = NULL;
#endif
#if defined(SWIFT_DEBUG_CHECKS) || defined(SWIFT_CELL_GRAPH)
        cell_assign_top_level_cell_index(c, s);
#endif
      }
    }
  }

  /* We need to label the zoom region's background cell as void. */
  c = &s->cells_top[s->zoom_props->void_cell_index];
  c->tl_cell_type = void_tl_cell;

#ifdef SWIFT_DEBUG_CHECKS
  /* Lets check all the cells are in the right place with the correct widths */
  debug_cell_type(s);
#endif

  /* Now find what cells neighbour the zoom region. */
  if (s->with_zoom_region)
    find_neighbouring_cells(s, gravity_properties, verbose);

#endif
}

/**
 * @brief Find what TL cells surround the zoom region.
 *
 * When interacting "natural" TL cells and "zoom" TL cells, it helps to know
 * what natural TL cells surround the zoom region. These cells then get tagged
 * as "tl_cell_neighbour".
 *
 * @param s The space.
 * @param verbose Are we talking?
 */
void find_neighbouring_cells(struct space *s,
                             struct gravity_props *gravity_properties,
                             const int verbose) {
#ifdef WITH_ZOOM_REGION
  const int cdim[3] = {s->cdim[0], s->cdim[1], s->cdim[2]};
  const int periodic = s->periodic;
  struct cell *cells = s->cells_top;

  /* Some info about the zoom domain */
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;

  /* Get some info about the physics */
  const double theta_crit_inv = 1. / gravity_properties->theta_crit;

  /* Maximal distance from shifted CoM to any corner */
  const double distance = 2. * cells[bkg_cell_offset].width[0] * theta_crit_inv;

  /* Compute how many cells away we need to walk */
  const int delta_cells = (int)(distance / cells[bkg_cell_offset].dmin) + 1;

  /* Turn this into upper and lower bounds for loops */
  const int delta_m = delta_cells;
  const int delta_p = delta_cells;

  /* Allocate the indices of neighbour background cells */
  if (swift_memalign("neighbour_cells_top",
                     (void **)&s->zoom_props->neighbour_cells_top,
                     SWIFT_STRUCT_ALIGNMENT,
                     s->zoom_props->nr_bkg_cells * sizeof(int)) != 0)
    error("Failed to allocate indices of local top-level background cells.");
  bzero(s->zoom_props->neighbour_cells_top,
        s->zoom_props->nr_bkg_cells * sizeof(int));

  int neighbour_count = 0;
  int void_count = 0;

  /* Let's be verbose about this choice */
  if (verbose)
    message(
        "Looking for neighbouring natural cells up to %d natural top-level "
        "cells away from the zoom region (delta_m=%d "
        "delta_p=%d)",
        delta_cells, delta_m, delta_p);

  /* Loop over each cell in the space to find the neighbouring top level cells
   * surrounding the zoom region. */
  for (int i = 0; i < cdim[0]; i++) {
    for (int j = 0; j < cdim[1]; j++) {
      for (int k = 0; k < cdim[2]; k++) {

        /* Get the cell ID. */
        const int cid = cell_getid(cdim, i, j, k) + bkg_cell_offset;

#ifdef SWIFT_DEBUG_CHECKS
        /* Ensure all background cells are actually background cells */
        if (cells[cid].width[0] == s->zoom_props->width[0])
          error("A zoom cell has been given a natural cell label!");
#endif

        /* Only interested in cells hosting zoom top level cells. */
        if (cells[cid].tl_cell_type != void_tl_cell) continue;

        void_count++;

        /* Loop over all its direct neighbours. */
        for (int ii = -delta_m; ii <= delta_p; ii++) {
          int iii = i + ii;
          if (!periodic && (iii < 0 || iii >= cdim[0])) continue;
          iii = (iii + cdim[0]) % cdim[0];
          for (int jj = -delta_m; jj <= delta_p; jj++) {
            int jjj = j + jj;
            if (!periodic && (jjj < 0 || jjj >= cdim[1])) continue;
            jjj = (jjj + cdim[1]) % cdim[1];
            for (int kk = -delta_m; kk <= delta_p; kk++) {
              int kkk = k + kk;
              if (!periodic && (kkk < 0 || kkk >= cdim[2])) continue;
              kkk = (kkk + cdim[2]) % cdim[2];

              /* Get the cell ID of the neighbour. */
              const int cjd = cell_getid(cdim, iii, jjj, kkk) + bkg_cell_offset;

              if (cells[cjd].tl_cell_type == tl_cell) {

                /* Record that we've found a neighbour. */
                cells[cjd].tl_cell_type = tl_cell_neighbour;
                s->zoom_props->neighbour_cells_top[neighbour_count] = cjd;
                neighbour_count++;
              }
            }
          }
        }
      }
    }
  }

  /* Store the number of neighbour cells */
  s->zoom_props->nr_neighbour_cells = neighbour_count;

  if (verbose) {
    message("%i cells neighbouring the zoom region", neighbour_count);
    message("%i void cells in the zoom region", void_count);
  }
#endif
}

/**
 * @brief Minimum distance between two TL cells with different sizes.
 *
 * @param ci, cj The two TL cells.
 * @param periodic Account for periodicity?
 * @param dim The boxsize.
 */
double cell_min_dist2_diff_size(const struct cell *restrict ci,
                                const struct cell *restrict cj,
                                const int periodic, const double dim[3]) {
#ifdef WITH_ZOOM_REGION

#ifdef SWIFT_DEBUG_CHECKS
  if (ci->width[0] == cj->width[0]) error("x cells of same size!");
  if (ci->width[1] == cj->width[1]) error("y cells of same size!");
  if (ci->width[2] == cj->width[2]) error("z cells of same size!");
#endif

  const double cix = ci->loc[0] + ci->width[0] / 2.;
  const double ciy = ci->loc[1] + ci->width[1] / 2.;
  const double ciz = ci->loc[2] + ci->width[2] / 2.;

  const double cjx = cj->loc[0] + cj->width[0] / 2.;
  const double cjy = cj->loc[1] + cj->width[1] / 2.;
  const double cjz = cj->loc[2] + cj->width[2] / 2.;

  const double diag_ci2 = ci->width[0] * ci->width[0] +
                          ci->width[1] * ci->width[1] +
                          ci->width[2] * ci->width[2];
  const double diag_cj2 = cj->width[0] * cj->width[0] +
                          cj->width[1] * cj->width[1] +
                          cj->width[2] * cj->width[2];

  /* Get the distance between the cells */
  double dx = cix - cjx;
  double dy = ciy - cjy;
  double dz = ciz - cjz;

  /* Apply BC */
  if (periodic) {
    dx = nearest(dx, dim[0]);
    dy = nearest(dy, dim[1]);
    dz = nearest(dz, dim[2]);
  }
  const double r2 = dx * dx + dy * dy + dz * dz;

  /* Minimal distance between any 2 particles in the two cells */
  const double dist2 = r2 - (diag_ci2 / 2. + diag_cj2 / 2.);

  return dist2;
#else
  return 0;
#endif
}

/**
 * @brief Minimum distance between two TL cells.
 *
 * Generic wrapper, don't know if the TL cells are the same size or not at time
 * of calling.
 *
 * @param ci, cj The two TL cells.
 * @param periodic Account for periodicity?
 * @param dim The boxsize.
 */
double cell_min_dist2(const struct cell *restrict ci,
                      const struct cell *restrict cj, const int periodic,
                      const double dim[3]) {
#ifdef WITH_ZOOM_REGION
  double dist2;

  /* Two natural TL cells. */
  if (ci->tl_cell_type <= 2 && cj->tl_cell_type <= 2) {
    dist2 = cell_min_dist2_same_size(ci, cj, periodic, dim);
    /* Two zoom TL cells. */
  } else if (ci->tl_cell_type == zoom_tl_cell &&
             cj->tl_cell_type == zoom_tl_cell) {
    dist2 = cell_min_dist2_same_size(ci, cj, periodic, dim);
    /* A mix of natural and zoom TL cells. */
  } else {
    dist2 = cell_min_dist2_diff_size(ci, cj, periodic, dim);
  }

  return dist2;
#else
  return 0;
#endif
}

#ifdef WITH_ZOOM_REGION
/**
 * @brief Create and fill the proxies for the natural cells.
 *
 * @param e The #engine.
 */
void engine_makeproxies_natural_cells(struct engine *e) {
#ifdef WITH_MPI
  /* Useful local information */
  const int nodeID = e->nodeID;
  const struct space *s = e->s;

  /* Handle on the cells and proxies */
  struct cell *cells = s->cells_top;
  struct proxy *proxies = e->proxies;

  /* Some info about the zoom domain */
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;

  /* Some info about the domain */
  const int cdim[3] = {s->cdim[0], s->cdim[1], s->cdim[2]};
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const int periodic = s->periodic;
  const double cell_width[3] = {cells[bkg_cell_offset].width[0],
                                cells[bkg_cell_offset].width[1],
                                cells[bkg_cell_offset].width[2]};

  /* Get some info about the physics */
  const int with_hydro = (e->policy & engine_policy_hydro);
  const int with_gravity = (e->policy & engine_policy_self_gravity);
  const double theta_crit = e->gravity_properties->theta_crit;
  const double theta_crit_inv = 1. / e->gravity_properties->theta_crit;
  const double max_mesh_dist = e->mesh->r_cut_max;
  const double max_mesh_dist2 = max_mesh_dist * max_mesh_dist;

  /* Distance between centre of the cell and corners */
  const double r_diag2 = cell_width[0] * cell_width[0] +
                         cell_width[1] * cell_width[1] +
                         cell_width[2] * cell_width[2];
  const double r_diag = 0.5 * sqrt(r_diag2);

  /* Maximal distance from shifted CoM to any corner */
  const double r_max = 2 * r_diag;

  /* Compute how many cells away we need to walk */
  int delta_cells = 1; /*hydro case */

  /* Gravity needs to take the opening angle into account */
  if (with_gravity) {
    const double distance = 2. * r_max * theta_crit_inv;
    delta_cells = (int)(distance / cells[bkg_cell_offset].dmin) + 1;
  }

  /* Turn this into upper and lower bounds for loops */
  int delta_m = delta_cells;
  int delta_p = delta_cells;

  /* Special case where every cell is in range of every other one */
  if (delta_cells >= cdim[0] / 2) {
    if (cdim[0] % 2 == 0) {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2 - 1;
    } else {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2;
    }
  }

  /* Let's be verbose about this choice */
  if (e->verbose)
    message(
        "Looking for proxies up to %d natural top-level cells away (delta_m=%d "
        "delta_p=%d)",
        delta_cells, delta_m, delta_p);

  /* Loop over each cell in the space. */
  for (int i = 0; i < cdim[0]; i++) {
    for (int j = 0; j < cdim[1]; j++) {
      for (int k = 0; k < cdim[2]; k++) {

        /* Get the cell ID. */
        const int cid = cell_getid(cdim, i, j, k) + bkg_cell_offset;

        /* Loop over all its neighbours neighbours in range. */
        for (int ii = -delta_m; ii <= delta_p; ii++) {
          int iii = i + ii;
          if (!periodic && (iii < 0 || iii >= cdim[0])) continue;
          iii = (iii + cdim[0]) % cdim[0];
          for (int jj = -delta_m; jj <= delta_p; jj++) {
            int jjj = j + jj;
            if (!periodic && (jjj < 0 || jjj >= cdim[1])) continue;
            jjj = (jjj + cdim[1]) % cdim[1];
            for (int kk = -delta_m; kk <= delta_p; kk++) {
              int kkk = k + kk;
              if (!periodic && (kkk < 0 || kkk >= cdim[2])) continue;
              kkk = (kkk + cdim[2]) % cdim[2];

              /* Get the cell ID. */
              const int cjd = cell_getid(cdim, iii, jjj, kkk) + bkg_cell_offset;

              /* Early abort  */
              if (cid >= cjd) continue;

              /* Early abort (both same node) */
              if (cells[cid].nodeID == nodeID && cells[cjd].nodeID == nodeID)
                continue;

              /* Early abort (both foreign node) */
              if (cells[cid].nodeID != nodeID && cells[cjd].nodeID != nodeID)
                continue;

              int proxy_type = 0;

              /* In the hydro case, only care about direct neighbours */
              if (with_hydro) {

                // MATTHIEU: to do: Write a better expression for the
                // non-periodic case.

                /* This is super-ugly but checks for direct neighbours */
                /* with periodic BC */
                if (((abs(i - iii) <= 1 || abs(i - iii - cdim[0]) <= 1 ||
                      abs(i - iii + cdim[0]) <= 1) &&
                     (abs(j - jjj) <= 1 || abs(j - jjj - cdim[1]) <= 1 ||
                      abs(j - jjj + cdim[1]) <= 1) &&
                     (abs(k - kkk) <= 1 || abs(k - kkk - cdim[2]) <= 1 ||
                      abs(k - kkk + cdim[2]) <= 1)))
                  proxy_type |= (int)proxy_cell_type_hydro;
              }

              /* In the gravity case, check distances using the MAC. */
              if (with_gravity) {

                /* First just add the direct neighbours. Then look for
                   some further out if the opening angle demands it */

                /* This is super-ugly but checks for direct neighbours */
                /* with periodic BC */
                if (((abs(i - iii) <= 1 || abs(i - iii - cdim[0]) <= 1 ||
                      abs(i - iii + cdim[0]) <= 1) &&
                     (abs(j - jjj) <= 1 || abs(j - jjj - cdim[1]) <= 1 ||
                      abs(j - jjj + cdim[1]) <= 1) &&
                     (abs(k - kkk) <= 1 || abs(k - kkk - cdim[2]) <= 1 ||
                      abs(k - kkk + cdim[2]) <= 1))) {

                  proxy_type |= (int)proxy_cell_type_gravity;
                } else {

                  /* We don't have multipoles yet (or their CoMs) so we will
                     have to cook up something based on cell locations only. We
                     hence need a lower limit on the distance that the CoMs in
                     those cells could have and an upper limit on the distance
                     of the furthest particle in the multipole from its CoM.
                     We then can decide whether we are too close for an M2L
                     interaction and hence require a proxy as this pair of cells
                     cannot rely on just an M2L calculation. */

                  /* Minimal distance between any two points in the cells */
                  const double min_dist_CoM2 = cell_min_dist2_same_size(
                      &cells[cid], &cells[cjd], periodic, dim);

                  /* Are we beyond the distance where the truncated forces are 0
                   * but not too far such that M2L can be used? */
                  if (periodic) {

                    if ((min_dist_CoM2 < max_mesh_dist2) &&
                        !(4. * r_max * r_max <
                          theta_crit * theta_crit * min_dist_CoM2))
                      proxy_type |= (int)proxy_cell_type_gravity;

                  } else {

                    if (!(4. * r_max * r_max <
                          theta_crit * theta_crit * min_dist_CoM2)) {
                      proxy_type |= (int)proxy_cell_type_gravity;
                    }
                  }
                }
              }

              /* Abort if not in range at all */
              if (proxy_type == proxy_cell_type_none) continue;

              /* Add to proxies? */
              if (cells[cid].nodeID == nodeID && cells[cjd].nodeID != nodeID) {

                /* Do we already have a relationship with this node? */
                int proxy_id = e->proxy_ind[cells[cjd].nodeID];
                if (proxy_id < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");

                  /* Ok, start a new proxy for this pair of nodes */
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cjd].nodeID);

                  /* Store the information */
                  e->proxy_ind[cells[cjd].nodeID] = e->nr_proxies;
                  proxy_id = e->nr_proxies;
                  e->nr_proxies += 1;

                  /* Check the maximal proxy limit */
                  if ((size_t)proxy_id > 8 * sizeof(long long))
                    error(
                        "Created more than %zd proxies. cell.mpi.sendto will "
                        "overflow.",
                        8 * sizeof(long long));
                }

                /* Add the cell to the proxy */
                proxy_addcell_in(&proxies[proxy_id], &cells[cjd], proxy_type);
                proxy_addcell_out(&proxies[proxy_id], &cells[cid], proxy_type);

                /* Store info about where to send the cell */
                cells[cid].mpi.sendto |= (1ULL << proxy_id);
              }

              /* Same for the symmetric case? */
              if (cells[cjd].nodeID == nodeID && cells[cid].nodeID != nodeID) {

                /* Do we already have a relationship with this node? */
                int proxy_id = e->proxy_ind[cells[cid].nodeID];
                if (proxy_id < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");

                  /* Ok, start a new proxy for this pair of nodes */
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cid].nodeID);

                  /* Store the information */
                  e->proxy_ind[cells[cid].nodeID] = e->nr_proxies;
                  proxy_id = e->nr_proxies;
                  e->nr_proxies += 1;

                  /* Check the maximal proxy limit */
                  if ((size_t)proxy_id > 8 * sizeof(long long))
                    error(
                        "Created more than %zd proxies. cell.mpi.sendto will "
                        "overflow.",
                        8 * sizeof(long long));
                }

                /* Add the cell to the proxy */
                proxy_addcell_in(&proxies[proxy_id], &cells[cid], proxy_type);
                proxy_addcell_out(&proxies[proxy_id], &cells[cjd], proxy_type);

                /* Store info about where to send the cell */
                cells[cjd].mpi.sendto |= (1ULL << proxy_id);
              }
            }
          }
        }
      }
    }
  }
#endif /* WITH_MPI */
}

/**
 * @brief Create and fill the proxies for the zoom cells.
 *
 * @param e The #engine.
 */
void engine_makeproxies_zoom_cells(struct engine *e) {
#ifdef WITH_MPI
  /* Useful local information */
  const int nodeID = e->nodeID;
  const struct space *s = e->s;

  /* Handle on the cells and proxies */
  struct cell *cells = s->cells_top;
  struct proxy *proxies = e->proxies;

  /* Some info about the domain */
  const int cdim[3] = {s->zoom_props->cdim[0], s->zoom_props->cdim[1],
                       s->zoom_props->cdim[2]};
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const int periodic = 0;
  const double cell_width[3] = {cells[0].width[0], cells[0].width[1],
                                cells[0].width[2]};

  /* Get some info about the physics */
  const int with_hydro = (e->policy & engine_policy_hydro);
  const int with_gravity = (e->policy & engine_policy_self_gravity);
  const double theta_crit = e->gravity_properties->theta_crit;
  const double theta_crit_inv = 1. / e->gravity_properties->theta_crit;
  const double max_mesh_dist = e->mesh->r_cut_max;
  const double max_mesh_dist2 = max_mesh_dist * max_mesh_dist;

  /* Distance between centre of the cell and corners */
  const double r_diag2 = cell_width[0] * cell_width[0] +
                         cell_width[1] * cell_width[1] +
                         cell_width[2] * cell_width[2];
  const double r_diag = 0.5 * sqrt(r_diag2);

  /* Maximal distance from shifted CoM to any corner */
  const double r_max = 2 * r_diag;

  /* Compute how many cells away we need to walk */
  int delta_cells = 1; /*hydro case */

  /* Gravity needs to take the opening angle into account */
  if (with_gravity) {
    const double distance = 2. * r_max * theta_crit_inv;
    delta_cells = (int)(distance / cells[0].dmin) + 1;
  }

  /* Turn this into upper and lower bounds for loops */
  int delta_m = delta_cells;
  int delta_p = delta_cells;

  /* Special case where every cell is in range of every other one */
  if (delta_cells >= cdim[0] / 2) {
    if (cdim[0] % 2 == 0) {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2 - 1;
    } else {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2;
    }
  }

  /* Let's be verbose about this choice */
  if (e->verbose)
    message(
        "Looking for proxies up to %d zoom top-level cells away (delta_m=%d "
        "delta_p=%d)",
        delta_cells, delta_m, delta_p);

  /* Loop over each cell in the space. */
  for (int i = 0; i < cdim[0]; i++) {
    for (int j = 0; j < cdim[1]; j++) {
      for (int k = 0; k < cdim[2]; k++) {

        /* Get the cell ID. */
        const int cid = cell_getid(cdim, i, j, k);

        /* Loop over all its neighbours neighbours in range. */
        for (int ii = -delta_m; ii <= delta_p; ii++) {
          int iii = i + ii;
          if (!periodic && (iii < 0 || iii >= cdim[0])) continue;
          iii = (iii + cdim[0]) % cdim[0];
          for (int jj = -delta_m; jj <= delta_p; jj++) {
            int jjj = j + jj;
            if (!periodic && (jjj < 0 || jjj >= cdim[1])) continue;
            jjj = (jjj + cdim[1]) % cdim[1];
            for (int kk = -delta_m; kk <= delta_p; kk++) {
              int kkk = k + kk;
              if (!periodic && (kkk < 0 || kkk >= cdim[2])) continue;
              kkk = (kkk + cdim[2]) % cdim[2];

              /* Get the cell ID. */
              const int cjd = cell_getid(cdim, iii, jjj, kkk);

              /* Early abort  */
              if (cid >= cjd) continue;

              /* Early abort (both same node) */
              if (cells[cid].nodeID == nodeID && cells[cjd].nodeID == nodeID)
                continue;

              /* Early abort (both foreign node) */
              if (cells[cid].nodeID != nodeID && cells[cjd].nodeID != nodeID)
                continue;

              int proxy_type = 0;

              /* In the hydro case, only care about direct neighbours */
              if (with_hydro) {

                /* Check for direct neighbours without periodic BC */
                if ((abs(i - iii) <= 1) && (abs(j - jjj) <= 1) &&
                    (abs(k - kkk) <= 1))
                  proxy_type |= (int)proxy_cell_type_hydro;
              }

              /* In the gravity case, check distances using the MAC. */
              if (with_gravity) {

                /* First just add the direct neighbours. Then look for
                   some further out if the opening angle demands it */

                /* Check for direct neighbours without periodic BC */
                if ((abs(i - iii) <= 1) && (abs(j - jjj) <= 1) &&
                    (abs(k - kkk) <= 1)) {

                  proxy_type |= (int)proxy_cell_type_gravity;

                } else {

                  /* We don't have multipoles yet (or their CoMs) so we will
                     have to cook up something based on cell locations only. We
                     hence need a lower limit on the distance that the CoMs in
                     those cells could have and an upper limit on the distance
                     of the furthest particle in the multipole from its CoM.
                     We then can decide whether we are too close for an M2L
                     interaction and hence require a proxy as this pair of cells
                     cannot rely on just an M2L calculation. */

                  /* Minimal distance between any two points in the cells */
                  const double min_dist_CoM2 = cell_min_dist2_same_size(
                      &cells[cid], &cells[cjd], periodic, dim);

                  /* Are we beyond the distance where the truncated forces are 0
                   * but not too far such that M2L can be used? */
                  if (periodic) {

                    if ((min_dist_CoM2 < max_mesh_dist2) &&
                        !(4. * r_max * r_max <
                          theta_crit * theta_crit * min_dist_CoM2))
                      proxy_type |= (int)proxy_cell_type_gravity;

                  } else {

                    if (!(4. * r_max * r_max <
                          theta_crit * theta_crit * min_dist_CoM2)) {
                      proxy_type |= (int)proxy_cell_type_gravity;
                    }
                  }
                }
              }

              /* Abort if not in range at all */
              if (proxy_type == proxy_cell_type_none) continue;

              /* Add to proxies? */
              if (cells[cid].nodeID == nodeID && cells[cjd].nodeID != nodeID) {

                /* Do we already have a relationship with this node? */
                int proxy_id = e->proxy_ind[cells[cjd].nodeID];
                if (proxy_id < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");

                  /* Ok, start a new proxy for this pair of nodes */
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cjd].nodeID);

                  /* Store the information */
                  e->proxy_ind[cells[cjd].nodeID] = e->nr_proxies;
                  proxy_id = e->nr_proxies;
                  e->nr_proxies += 1;

                  /* Check the maximal proxy limit */
                  if ((size_t)proxy_id > 8 * sizeof(long long))
                    error(
                        "Created more than %zd proxies. cell.mpi.sendto will "
                        "overflow.",
                        8 * sizeof(long long));
                }

                /* Add the cell to the proxy */
                proxy_addcell_in(&proxies[proxy_id], &cells[cjd], proxy_type);
                proxy_addcell_out(&proxies[proxy_id], &cells[cid], proxy_type);

                /* Store info about where to send the cell */
                cells[cid].mpi.sendto |= (1ULL << proxy_id);
              }

              /* Same for the symmetric case? */
              if (cells[cjd].nodeID == nodeID && cells[cid].nodeID != nodeID) {

                /* Do we already have a relationship with this node? */
                int proxy_id = e->proxy_ind[cells[cid].nodeID];
                if (proxy_id < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");

                  /* Ok, start a new proxy for this pair of nodes */
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cid].nodeID);

                  /* Store the information */
                  e->proxy_ind[cells[cid].nodeID] = e->nr_proxies;
                  proxy_id = e->nr_proxies;
                  e->nr_proxies += 1;

                  /* Check the maximal proxy limit */
                  if ((size_t)proxy_id > 8 * sizeof(long long))
                    error(
                        "Created more than %zd proxies. cell.mpi.sendto will "
                        "overflow.",
                        8 * sizeof(long long));
                }

                /* Add the cell to the proxy */
                proxy_addcell_in(&proxies[proxy_id], &cells[cid], proxy_type);
                proxy_addcell_out(&proxies[proxy_id], &cells[cjd], proxy_type);

                /* Store info about where to send the cell */
                cells[cjd].mpi.sendto |= (1ULL << proxy_id);
              }
            }
          }
        }
      }
    }
  }
#endif /* WITH_MPI */
}

/**
 * @brief Create and fill the proxies for relations between cell grids.
 *
 * This is done "lazily" by just making proxies for all neighbour cells.
 *
 * @param e The #engine.
 */
void engine_makeproxies_between_grids(struct engine *e) {
#ifdef WITH_MPI
  /* Useful local information */
  const int nodeID = e->nodeID;
  const struct space *s = e->s;

  /* Handle on the cells and proxies */
  struct cell *cells = s->cells_top;
  struct proxy *proxies = e->proxies;

  /* Some info about the zoom domain */
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;

  /* Some info about the domain */
  const int cdim[3] = {s->cdim[0], s->cdim[1], s->cdim[2]};
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const int periodic = s->periodic;
  const double cell_width[3] = {cells[bkg_cell_offset].width[0],
                                cells[bkg_cell_offset].width[1],
                                cells[bkg_cell_offset].width[2]};

  /* Get some info about the physics */
  const int with_hydro = (e->policy & engine_policy_hydro);
  const int with_gravity = (e->policy & engine_policy_self_gravity);
  const double theta_crit = e->gravity_properties->theta_crit;
  const double max_mesh_dist = e->mesh->r_cut_max;
  const double max_mesh_dist2 = max_mesh_dist * max_mesh_dist;

  /* Distance between centre of the cell and corners */
  const double r_diag2 = cell_width[0] * cell_width[0] +
                         cell_width[1] * cell_width[1] +
                         cell_width[2] * cell_width[2];
  const double r_diag = 0.5 * sqrt(r_diag2);

  /* Maximal distance from shifted CoM to any corner */
  const double r_max = 2 * r_diag;

  /* Loop over each zoom cell in the space. */
  for (int cid = 0; cid < bkg_cell_offset; cid++) {

    /* Integer indices of this cell in the natural parent */
    const int natural_tl_cid = cells[cid].parent_bkg_cid - bkg_cell_offset;
    const int i = natural_tl_cid / (cdim[1] * cdim[2]);
    const int j = (natural_tl_cid / cdim[2]) % cdim[1];
    const int k = natural_tl_cid % cdim[2];

    /* Loop over every cell in the natural grid */
    for (int cjd = bkg_cell_offset; cjd < s->nr_cells; cjd++) {

      /* We only want to consider background cells if they are neighbours */
      if (cells[cjd].tl_cell_type != tl_cell_neighbour) continue;

      /* Early abort (both same node) */
      if (cells[cid].nodeID == nodeID && cells[cjd].nodeID == nodeID) continue;

      /* Early abort (both foreign node) */
      if (cells[cid].nodeID != nodeID && cells[cjd].nodeID != nodeID) continue;

      /* Integer indices of the cell in the top-level grid */
      const int ii = (cjd - bkg_cell_offset) / (cdim[1] * cdim[2]);
      const int jj = ((cjd - bkg_cell_offset) / cdim[2]) % cdim[1];
      const int kk = (cjd - bkg_cell_offset) % cdim[2];

      int proxy_type = 0;

      /* In the hydro case, only care about direct neighbours */
      if (with_hydro) {

        // MATTHIEU: to do: Write a better expression for the
        // non-periodic case.

        /* This is super-ugly but checks for direct neighbours */
        /* with periodic BC */
        if (((abs(i - ii) <= 1 || abs(i - ii - cdim[0]) <= 1 ||
              abs(i - ii + cdim[0]) <= 1) &&
             (abs(j - jj) <= 1 || abs(j - jj - cdim[1]) <= 1 ||
              abs(j - jj + cdim[1]) <= 1) &&
             (abs(k - kk) <= 1 || abs(k - kk - cdim[2]) <= 1 ||
              abs(k - kk + cdim[2]) <= 1)))
          proxy_type |= (int)proxy_cell_type_hydro;
      }

      /* In the gravity case, check distances using the MAC. */
      if (with_gravity) {

        /* First just add the direct neighbours. Then look for
           some further out if the opening angle demands it */

        /* This is super-ugly but checks for direct neighbours */
        /* with periodic BC */
        if (((abs(i - ii) <= 1 || abs(i - ii - cdim[0]) <= 1 ||
              abs(i - ii + cdim[0]) <= 1) &&
             (abs(j - jj) <= 1 || abs(j - jj - cdim[1]) <= 1 ||
              abs(j - jj + cdim[1]) <= 1) &&
             (abs(k - kk) <= 1 || abs(k - kk - cdim[2]) <= 1 ||
              abs(k - kk + cdim[2]) <= 1))) {

          proxy_type |= (int)proxy_cell_type_gravity;
        } else {

          /* We don't have multipoles yet (or their CoMs) so we will
             have to cook up something based on cell locations only. We
             hence need a lower limit on the distance that the CoMs in
             those cells could have and an upper limit on the distance
             of the furthest particle in the multipole from its CoM.
             We then can decide whether we are too close for an M2L
             interaction and hence require a proxy as this pair of cells
             cannot rely on just an M2L calculation. */

          /* Minimal distance between any two points in the cells */
          const double min_dist_CoM2 =
              cell_min_dist2_diff_size(&cells[cid], &cells[cjd], periodic, dim);

          /* Are we beyond the distance where the truncated forces are 0
           * but not too far such that M2L can be used? */
          if (periodic) {

            if ((min_dist_CoM2 < max_mesh_dist2) &&
                !(4. * r_max * r_max < theta_crit * theta_crit * min_dist_CoM2))
              proxy_type |= (int)proxy_cell_type_gravity;

          } else {

            if (!(4. * r_max * r_max <
                  theta_crit * theta_crit * min_dist_CoM2)) {
              proxy_type |= (int)proxy_cell_type_gravity;
            }
          }
        }
      }

      /* Abort if not in range at all */
      if (proxy_type == proxy_cell_type_none) continue;

      /* Add to proxies? */
      if (cells[cid].nodeID == nodeID && cells[cjd].nodeID != nodeID) {

        /* Do we already have a relationship with this node? */
        int proxy_id = e->proxy_ind[cells[cjd].nodeID];
        if (proxy_id < 0) {
          if (e->nr_proxies == engine_maxproxies)
            error("Maximum number of proxies exceeded.");

          /* Ok, start a new proxy for this pair of nodes */
          proxy_init(&proxies[e->nr_proxies], e->nodeID, cells[cjd].nodeID);

          /* Store the information */
          e->proxy_ind[cells[cjd].nodeID] = e->nr_proxies;
          proxy_id = e->nr_proxies;
          e->nr_proxies += 1;

          /* Check the maximal proxy limit */
          if ((size_t)proxy_id > 8 * sizeof(long long))
            error(
                "Created more than %zd proxies. cell.mpi.sendto will "
                "overflow.",
                8 * sizeof(long long));
        }

        /* Add the cell to the proxy */
        proxy_addcell_in(&proxies[proxy_id], &cells[cjd], proxy_type);
        proxy_addcell_out(&proxies[proxy_id], &cells[cid], proxy_type);

        /* Store info about where to send the cell */
        cells[cid].mpi.sendto |= (1ULL << proxy_id);
      }

      /* Same for the symmetric case? */
      if (cells[cjd].nodeID == nodeID && cells[cid].nodeID != nodeID) {

        /* Do we already have a relationship with this node? */
        int proxy_id = e->proxy_ind[cells[cid].nodeID];
        if (proxy_id < 0) {
          if (e->nr_proxies == engine_maxproxies)
            error("Maximum number of proxies exceeded.");

          /* Ok, start a new proxy for this pair of nodes */
          proxy_init(&proxies[e->nr_proxies], e->nodeID, cells[cid].nodeID);

          /* Store the information */
          e->proxy_ind[cells[cid].nodeID] = e->nr_proxies;
          proxy_id = e->nr_proxies;
          e->nr_proxies += 1;

          /* Check the maximal proxy limit */
          if ((size_t)proxy_id > 8 * sizeof(long long))
            error(
                "Created more than %zd proxies. cell.mpi.sendto will "
                "overflow.",
                8 * sizeof(long long));
        }

        /* Add the cell to the proxy */
        proxy_addcell_in(&proxies[proxy_id], &cells[cid], proxy_type);
        proxy_addcell_out(&proxies[proxy_id], &cells[cjd], proxy_type);

        /* Store info about where to send the cell */
        cells[cjd].mpi.sendto |= (1ULL << proxy_id);
      }
    }
  }
#endif /* WITH_MPI */
}

/**
 * @brief Create and fill the proxies including the zoom region.
 *
 * This replaces the function in engine_proxy when running with a zoom region.
 *
 * @param e The #engine.
 */
void engine_makeproxies_with_zoom_region(struct engine *e) {
#ifdef WITH_MPI

  /* Let's time this */
  const ticks tic = getticks();

  /* Prepare the proxies and the proxy index. */
  if (e->proxy_ind == NULL)
    if ((e->proxy_ind = (int *)malloc(sizeof(int) * e->nr_nodes)) == NULL)
      error("Failed to allocate proxy index.");
  for (int k = 0; k < e->nr_nodes; k++) e->proxy_ind[k] = -1;
  e->nr_proxies = 0;

  engine_makeproxies_zoom_cells(e);
  engine_makeproxies_natural_cells(e);
  engine_makeproxies_between_grids(e);

  /* Be clear about the time */
  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Constructs the top-level tasks for the short-range gravity
 * and long-range gravity interactions for the natural/background cells.
 *
 * - All top-cells get a self task.
 * - All pairs within range according to the multipole acceptance
 *   criterion get a pair task.
 */
void engine_make_self_gravity_tasks_mapper_natural_cells(void *map_data,
                                                         int num_elements,
                                                         void *extra_data) {

  struct engine *e = (struct engine *)extra_data;
  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int periodic = s->periodic;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const int cdim[3] = {s->cdim[0], s->cdim[1], s->cdim[2]};
  struct cell *cells = s->cells_top;
  const double theta_crit = e->gravity_properties->theta_crit;
  const double max_distance = e->mesh->r_cut_max;
  const double max_distance2 = max_distance * max_distance;

  /* Some info about the zoom domain */
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;

  /* Compute how many cells away we need to walk */
  const double distance = 2.5 * cells[bkg_cell_offset].width[0] / theta_crit;
  int delta = (int)(distance / cells[bkg_cell_offset].width[0]) + 1;
  int delta_m = delta;
  int delta_p = delta;

  /* Special case where every cell is in range of every other one */
  if (delta >= cdim[0] / 2) {
    if (cdim[0] % 2 == 0) {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2 - 1;
    } else {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2;
    }
  }

  /* Loop through the elements, which are just byte offsets from NULL. */
  for (int ind = 0; ind < num_elements; ind++) {

    /* Get the cell index, including background cell offset. */
    const int cid = (size_t)(map_data) + ind + bkg_cell_offset;

    /* Integer indices of the cell in the top-level grid */
    const int i = (cid - bkg_cell_offset) / (cdim[1] * cdim[2]);
    const int j = ((cid - bkg_cell_offset) / cdim[2]) % cdim[1];
    const int k = (cid - bkg_cell_offset) % cdim[2];

    /* Get the cell */
    struct cell *ci = &cells[cid];

    /* Skip cells without gravity particles */
    if (ci->grav.count == 0) continue;

    /* Ensure we haven't found a void cell with particles */
    if (ci->tl_cell_type == void_tl_cell)
      error("This void cell (cid=%d) has got particles!", cid);

    /* If the cell is local build a self-interaction */
    if (ci->nodeID == nodeID) {
      scheduler_addtask(sched, task_type_self, task_subtype_grav, 0, 0, ci,
                        NULL);
    }

    /* Loop over every other cell within (Manhattan) range delta */
    for (int ii = -delta_m; ii <= delta_p; ii++) {
      int iii = i + ii;
      if (!periodic && (iii < 0 || iii >= cdim[0])) continue;
      iii = (iii + cdim[0]) % cdim[0];
      for (int jj = -delta_m; jj <= delta_p; jj++) {
        int jjj = j + jj;
        if (!periodic && (jjj < 0 || jjj >= cdim[1])) continue;
        jjj = (jjj + cdim[1]) % cdim[1];
        for (int kk = -delta_m; kk <= delta_p; kk++) {
          int kkk = k + kk;
          if (!periodic && (kkk < 0 || kkk >= cdim[2])) continue;
          kkk = (kkk + cdim[2]) % cdim[2];

          /* Get the cell */
          const int cjd = cell_getid(cdim, iii, jjj, kkk) + bkg_cell_offset;
          struct cell *cj = &cells[cjd];

          /* Avoid duplicates, empty cells and completely foreign pairs */
          if (cid >= cjd || cj->grav.count == 0 ||
              (ci->nodeID != nodeID && cj->nodeID != nodeID))
            continue;

          /* Recover the multipole information */
          const struct gravity_tensors *multi_i = ci->grav.multipole;
          const struct gravity_tensors *multi_j = cj->grav.multipole;

          if (multi_i == NULL && ci->nodeID != nodeID)
            error(
                "Multipole of ci was not exchanged properly via the proxies "
                "(nodeID=%d, ci->nodeID=%d)",
                nodeID, ci->nodeID);
          if (multi_j == NULL && cj->nodeID != nodeID)
            error(
                "Multipole of cj was not exchanged properly via the proxies "
                "(nodeID=%d, cj->nodeID=%d)",
                nodeID, cj->nodeID);

          /* Minimal distance between any pair of particles */
          const double min_radius2 =
              cell_min_dist2_same_size(ci, cj, periodic, dim);

          /* Are we beyond the distance where the truncated forces are 0 ?*/
          if (periodic && min_radius2 > max_distance2) continue;

          /* Are the cells too close for a MM interaction ? */
          if (!cell_can_use_pair_mm(ci, cj, e, s, /*use_rebuild_data=*/1,
                                    /*is_tree_walk=*/0)) {

            /* Ok, we need to add a direct pair calculation */
            scheduler_addtask(sched, task_type_pair, task_subtype_grav, 0, 0,
                              ci, cj);

#ifdef SWIFT_DEBUG_CHECKS
            /* Ensure both cells are background cells */
            if (ci->tl_cell_type == 3 || cj->tl_cell_type == 3) {
              error(
                  "Cell %d and cell %d are not background cells! "
                  "(ci->tl_cell_type=%d, cj->tl_cell_type=%d)",
                  cid, cjd, ci->tl_cell_type, cj->tl_cell_type);
            }
#ifdef WITH_MPI

            /* Let's cross-check that we had a proxy for that cell */
            if (ci->nodeID == nodeID && cj->nodeID != engine_rank) {

              /* Find the proxy for this node */
              const int proxy_id = e->proxy_ind[cj->nodeID];
              if (proxy_id < 0)
                error("No proxy exists for that foreign node %d!", cj->nodeID);

              const struct proxy *p = &e->proxies[proxy_id];

              /* Check whether the cell exists in the proxy */
              int n = 0;
              for (; n < p->nr_cells_in; n++)
                if (p->cells_in[n] == cj) {
                  break;
                }
              if (n == p->nr_cells_in)
                error(
                    "Cell %d not found in the proxy but trying to construct "
                    "grav task!",
                    cjd);
            } else if (cj->nodeID == nodeID && ci->nodeID != engine_rank) {

              /* Find the proxy for this node */
              const int proxy_id = e->proxy_ind[ci->nodeID];
              if (proxy_id < 0)
                error("No proxy exists for that foreign node %d!", ci->nodeID);

              const struct proxy *p = &e->proxies[proxy_id];

              /* Check whether the cell exists in the proxy */
              int n = 0;
              for (; n < p->nr_cells_in; n++)
                if (p->cells_in[n] == ci) {
                  break;
                }
              if (n == p->nr_cells_in)
                error(
                    "Cell %d not found in the proxy but trying to construct "
                    "grav task!",
                    cid);
            }
#endif /* WITH_MPI */
#endif /* SWIFT_DEBUG_CHECKS */
          }
        }
      }
    }
  }
}

/**
 * @brief Constructs the top-level tasks for the short-range gravity
 * and long-range gravity interactions.
 *
 * - All top-cells get a self task.
 * - All pairs within range according to the multipole acceptance
 *   criterion get a pair task.
 */
void engine_make_self_gravity_tasks_mapper_zoom_cells(void *map_data,
                                                      int num_elements,
                                                      void *extra_data) {

  struct engine *e = (struct engine *)extra_data;
  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int cdim[3] = {s->zoom_props->cdim[0], s->zoom_props->cdim[1],
                       s->zoom_props->cdim[2]};
  struct cell *cells = s->cells_top;
  const double theta_crit = e->gravity_properties->theta_crit;

  /* Compute how many cells away we need to walk */
  const double distance = 2.5 * cells[0].width[0] / theta_crit;
  int delta = (int)(distance / cells[0].width[0]) + 1;
  int delta_m = delta;
  int delta_p = delta;

  /* Special case where every cell is in range of every other one */
  if (delta >= cdim[0] / 2) {
    if (cdim[0] % 2 == 0) {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2 - 1;
    } else {
      delta_m = cdim[0] / 2;
      delta_p = cdim[0] / 2;
    }
  }

  /* Loop through the elements, which are just byte offsets from NULL. */
  for (int ind = 0; ind < num_elements; ind++) {

    /* Get the cell index. */
    const int cid = (size_t)(map_data) + ind;

    /* Integer indices of the cell in the top-level grid */
    const int i = cid / (cdim[1] * cdim[2]);
    const int j = (cid / cdim[2]) % cdim[1];
    const int k = cid % cdim[2];

    /* Get the cell */
    struct cell *ci = &cells[cid];

    /* Skip cells without gravity particles */
    if (ci->grav.count == 0) continue;

    /* If the cell is local build a self-interaction */
    if (ci->nodeID == nodeID) {
      scheduler_addtask(sched, task_type_self, task_subtype_grav, 0, 0, ci,
                        NULL);
    }

    /* Loop over every other cell within (Manhattan) range delta
     * NOTE: Zoom cells are never periodic */
    for (int ii = -delta_m; ii <= delta_p; ii++) {
      int iii = i + ii;
      if (iii < 0 || iii >= cdim[0]) continue;
      iii = (iii + cdim[0]) % cdim[0];
      for (int jj = -delta_m; jj <= delta_p; jj++) {
        int jjj = j + jj;
        if (jjj < 0 || jjj >= cdim[1]) continue;
        jjj = (jjj + cdim[1]) % cdim[1];
        for (int kk = -delta_m; kk <= delta_p; kk++) {
          int kkk = k + kk;
          if (kkk < 0 || kkk >= cdim[2]) continue;
          kkk = (kkk + cdim[2]) % cdim[2];

          /* Get the cell */
          const int cjd = cell_getid(cdim, iii, jjj, kkk);
          struct cell *cj = &cells[cjd];

          /* Avoid duplicates, empty cells and completely foreign pairs */
          if (cid >= cjd || cj->grav.count == 0 ||
              (ci->nodeID != nodeID && cj->nodeID != nodeID))
            continue;

          /* Recover the multipole information */
          const struct gravity_tensors *multi_i = ci->grav.multipole;
          const struct gravity_tensors *multi_j = cj->grav.multipole;

          if (multi_i == NULL && ci->nodeID != nodeID)
            error(
                "Multipole of ci was not exchanged properly via the proxies "
                "(nodeID=%d, ci->nodeID=%d)",
                nodeID, ci->nodeID);
          if (multi_j == NULL && cj->nodeID != nodeID)
            error(
                "Multipole of cj was not exchanged properly via the proxies "
                "(nodeID=%d, cj->nodeID=%d)",
                nodeID, cj->nodeID);

          /* Are the cells too close for a MM interaction ? */
          if (!cell_can_use_pair_mm(ci, cj, e, s, /*use_rebuild_data=*/1,
                                    /*is_tree_walk=*/0)) {

            /* Ok, we need to add a direct pair calculation */
            scheduler_addtask(sched, task_type_pair, task_subtype_grav, 0, 0,
                              ci, cj);

#ifdef SWIFT_DEBUG_CHECKS
            /* Ensure both cells are zoom cells */
            if (ci->tl_cell_type <= 2 || cj->tl_cell_type <= 2) {
              error(
                  "Cell %d and cell %d are not zoom cells! "
                  "(ci->tl_cell_type=%d, cj->tl_cell_type=%d)",
                  cid, cjd, ci->tl_cell_type, cj->tl_cell_type);
            }
#ifdef WITH_MPI

            /* Let's cross-check that we had a proxy for that cell */
            if (ci->nodeID == nodeID && cj->nodeID != engine_rank) {

              /* Find the proxy for this node */
              const int proxy_id = e->proxy_ind[cj->nodeID];
              if (proxy_id < 0)
                error("No proxy exists for that foreign node %d!", cj->nodeID);

              const struct proxy *p = &e->proxies[proxy_id];

              /* Check whether the cell exists in the proxy */
              int n = 0;
              for (; n < p->nr_cells_in; n++)
                if (p->cells_in[n] == cj) {
                  break;
                }
              if (n == p->nr_cells_in)
                error(
                    "Cell %d not found in the proxy but trying to construct "
                    "grav task!",
                    cjd);
            } else if (cj->nodeID == nodeID && ci->nodeID != engine_rank) {

              /* Find the proxy for this node */
              const int proxy_id = e->proxy_ind[ci->nodeID];
              if (proxy_id < 0)
                error("No proxy exists for that foreign node %d!", ci->nodeID);

              const struct proxy *p = &e->proxies[proxy_id];

              /* Check whether the cell exists in the proxy */
              int n = 0;
              for (; n < p->nr_cells_in; n++)
                if (p->cells_in[n] == ci) {
                  break;
                }
              if (n == p->nr_cells_in)
                error(
                    "Cell %d not found in the proxy but trying to construct "
                    "grav task!",
                    cid);
            }
#endif /* WITH_MPI */
#endif /* SWIFT_DEBUG_CHECKS */
          }
        }
      }
    }
  }
}

/**
 * @brief Constructs the top-level tasks for the short-range gravity
 * and long-range gravity interactions between natural level cells
 * and zoom level cells.
 *
 * This replaces the function in engine_maketasks when running with a zoom
 region.
 *
 * - All top-cells get a self task.
 * - All pairs of differing sized cells within range according to
 *   the multipole acceptance criterion get a pair task.
 *
 * @param map_data Offset of first two indices disguised as a pointer.
 * @param num_elements Number of cells to traverse.a
 * @param extra_data The #engine.

 */
void engine_make_self_gravity_tasks_mapper_with_zoom_diffsize(
    void *map_data, int num_elements, void *extra_data) {

  /* Useful local information */
  struct engine *e = (struct engine *)extra_data;
  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;

  /* Handle on the cells and proxies */
  struct cell *cells = s->cells_top;

  /* Some info about the zoom domain */
  const int bkg_cell_offset = s->zoom_props->tl_cell_offset;

  /* Some info about the domain */
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  int periodic = s->periodic;

  /* Get some info about the physics */
  const double max_mesh_dist = e->mesh->r_cut_max;
  const double max_mesh_dist2 = max_mesh_dist * max_mesh_dist;

  /* Define neighbour loop variables */
  int cjd_low = 0;
  int cjd_high = bkg_cell_offset;

  /* Loop through the elements, which are just byte offsets from NULL. */
  for (int ind = 0; ind < num_elements; ind++) {

    /* Get the cell index. */
    const int cid = (size_t)(map_data) + ind;

    /* Get the cell */
    struct cell *ci = &cells[cid];

    /* If this cell is on this node and is a background cell
     * then we have to avoid duplicating tasks */
    if (ci->nodeID == nodeID && ci->tl_cell_type <= 2) {
      continue;
    }

    /* Skip cells without gravity particles */
    if (ci->grav.count == 0) continue;

    /* If the cell is a natural cell and not a neighbour cell
     * we don't need to do anything */
    if ((ci->tl_cell_type <= 2) && (ci->tl_cell_type != tl_cell_neighbour)) {
      continue;
    }

    /* Get the loop range for the neighbouring cells */
    if (ci->tl_cell_type <= 2) {
      cjd_low = 0;
      cjd_high = bkg_cell_offset;
    } else {
      cjd_low = bkg_cell_offset;
      cjd_high = s->nr_cells;
    }

    /* Loop over every other cell */
    for (int cjd = cjd_low; cjd < cjd_high; cjd++) {

      /* Get the cell */
      struct cell *cj = &cells[cjd];

      /* If the cell is a natural cell and not a neighbour cell
       * we don't need to do anything */
      if ((cj->tl_cell_type <= 2) && (cj->tl_cell_type != tl_cell_neighbour)) {
        continue;
      }

      /* Avoid empty cells and completely foreign pairs */
      if (cj->grav.count == 0 || (ci->nodeID != nodeID && cj->nodeID != nodeID))
        continue;

      /* Explictly avoid duplicates */
      if ((ci->nodeID == cj->nodeID && cid >= cjd) ||
          (cj->nodeID == nodeID && cj->tl_cell_type == zoom_tl_cell)) {
        continue;
      }

      /* Recover the multipole information */
      const struct gravity_tensors *multi_i = ci->grav.multipole;
      const struct gravity_tensors *multi_j = cj->grav.multipole;

      if (multi_i == NULL && ci->nodeID != nodeID)
        error(
            "Multipole of ci was not exchanged properly via the proxies "
            "(nodeID=%d, ci->nodeID=%d)",
            nodeID, ci->nodeID);
      if (multi_j == NULL && cj->nodeID != nodeID)
        error(
            "Multipole of cj was not exchanged properly via the proxies "
            "(nodeID=%d, cj->nodeID=%d)",
            nodeID, cj->nodeID);

      /* Minimal distance between any pair of particles */
      const double min_radius2 =
          cell_min_dist2_diff_size(ci, cj, periodic, dim);

      /* Are we beyond the distance where the truncated forces are 0 ?*/
      if (periodic && min_radius2 > max_mesh_dist2) continue;

      /* Are the cells too close for a MM interaction ? */
      if (!cell_can_use_pair_mm(ci, cj, e, s, /*use_rebuild_data=*/1,
                                /*is_tree_walk=*/0)) {

        /* Ok, we need to add a direct pair calculation */
        scheduler_addtask(sched, task_type_pair, task_subtype_grav, 0, 0, ci,
                          cj);

#ifdef SWIFT_DEBUG_CHECKS
        /* Ensure both cells are not in the same level */
        if (((ci->tl_cell_type <= 2 && cj->tl_cell_type <= 2) ||
             (ci->tl_cell_type == cj->tl_cell_type))) {
          error(
              "Cell %d and cell %d are the same cell type! "
              "(ci->tl_cell_type=%d, cj->tl_cell_type=%d)",
              cid, cjd, ci->tl_cell_type, cj->tl_cell_type);
        }
#ifdef WITH_MPI

        /* Let's cross-check that we had a proxy for that cell */
        if (ci->nodeID == nodeID && cj->nodeID != engine_rank) {

          /* Find the proxy for this node */
          const int proxy_id = e->proxy_ind[cj->nodeID];
          if (proxy_id < 0)
            error("No proxy exists for that foreign node %d!", cj->nodeID);

          const struct proxy *p = &e->proxies[proxy_id];

          /* Check whether the cell exists in the proxy */
          int n = 0;
          for (; n < p->nr_cells_in; n++)
            if (p->cells_in[n] == cj) {
              break;
            }
          if (n == p->nr_cells_in)
            error(
                "Cell %d not found in the proxy but trying to construct "
                "grav task!",
                cjd);
        } else if (cj->nodeID == nodeID && ci->nodeID != engine_rank) {

          /* Find the proxy for this node */
          const int proxy_id = e->proxy_ind[ci->nodeID];
          if (proxy_id < 0)
            error("No proxy exists for that foreign node %d!", ci->nodeID);

          const struct proxy *p = &e->proxies[proxy_id];

          /* Check whether the cell exists in the proxy */
          int n = 0;
          for (; n < p->nr_cells_in; n++)
            if (p->cells_in[n] == ci) {
              break;
            }
          if (n == p->nr_cells_in)
            error(
                "Cell %d not found in the proxy but trying to construct "
                "grav task!",
                cid);
        }
#endif /* WITH_MPI */
#endif /* SWIFT_DEBUG_CHECKS */
      }
    }
  }
}

/**
 * @brief Constructs the top-level pair tasks for the first hydro loop over
 * neighbours
 *
 * Here we construct all the tasks for all possible neighbouring non-empty
 * local cells in the hierarchy. No dependencies are being added thus far.
 * Additional loop over neighbours can later be added by simply duplicating
 * all the tasks created by this function.
 *
 * This replaces the function in engine_maketasks but simply removes periodicity
 * (the zoom level never has periodicity), otherwise the function is identical.
 *
 * @param map_data Offset of first two indices disguised as a pointer.
 * @param num_elements Number of cells to traverse.
 * @param extra_data The #engine.
 */
void engine_make_hydroloop_tasks_mapper_with_zoom(void *map_data,
                                                  int num_elements,
                                                  void *extra_data) {

  /* Extract the engine pointer. */
  struct engine *e = (struct engine *)extra_data;
  const int with_feedback = (e->policy & engine_policy_feedback);
  const int with_stars = (e->policy & engine_policy_stars);
  const int with_sinks = (e->policy & engine_policy_sinks);
  const int with_black_holes = (e->policy & engine_policy_black_holes);

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int *cdim = s->zoom_props->cdim;
  struct cell *cells = s->cells_top;

  /* Loop through the elements, which are just byte offsets from NULL. */
  for (int ind = 0; ind < num_elements; ind++) {

    /* Get the cell index. */
    const int cid = (size_t)(map_data) + ind;

    /* Integer indices of the cell in the top-level grid */
    const int i = cid / (cdim[1] * cdim[2]);
    const int j = (cid / cdim[2]) % cdim[1];
    const int k = cid % cdim[2];

    /* Get the cell */
    struct cell *ci = &cells[cid];

    /* Skip cells without hydro or star particles */
    if ((ci->hydro.count == 0) && (!with_stars || ci->stars.count == 0) &&
        (!with_sinks || ci->sinks.count == 0) &&
        (!with_black_holes || ci->black_holes.count == 0))
      continue;

    /* If the cell is local build a self-interaction */
    if (ci->nodeID == nodeID) {
      scheduler_addtask(sched, task_type_self, task_subtype_density, 0, 0, ci,
                        NULL);
    }

    /* Now loop over all the neighbours of this cell */
    for (int ii = -1; ii < 2; ii++) {
      int iii = i + ii;
      if (iii < 0 || iii >= cdim[0]) continue;
      iii = (iii + cdim[0]) % cdim[0];
      for (int jj = -1; jj < 2; jj++) {
        int jjj = j + jj;
        if (jjj < 0 || jjj >= cdim[1]) continue;
        jjj = (jjj + cdim[1]) % cdim[1];
        for (int kk = -1; kk < 2; kk++) {
          int kkk = k + kk;
          if (kkk < 0 || kkk >= cdim[2]) continue;
          kkk = (kkk + cdim[2]) % cdim[2];

          /* Get the neighbouring cell */
          const int cjd = cell_getid(cdim, iii, jjj, kkk);
          struct cell *cj = &cells[cjd];

          /* Is that neighbour local and does it have gas or star particles ? */
          if ((cid >= cjd) ||
              ((cj->hydro.count == 0) &&
               (!with_feedback || cj->stars.count == 0) &&
               (!with_sinks || cj->sinks.count == 0) &&
               (!with_black_holes || cj->black_holes.count == 0)) ||
              (ci->nodeID != nodeID && cj->nodeID != nodeID))
            continue;

          /* Construct the pair task */
          const int sid = sortlistID[(kk + 1) + 3 * ((jj + 1) + 3 * (ii + 1))];
          scheduler_addtask(sched, task_type_pair, task_subtype_density, sid, 0,
                            ci, cj);

#ifdef SWIFT_DEBUG_CHECKS
#ifdef WITH_MPI

          /* Let's cross-check that we had a proxy for that cell */
          if (ci->nodeID == nodeID && cj->nodeID != engine_rank) {

            /* Find the proxy for this node */
            const int proxy_id = e->proxy_ind[cj->nodeID];
            if (proxy_id < 0)
              error("No proxy exists for that foreign node %d!", cj->nodeID);

            const struct proxy *p = &e->proxies[proxy_id];

            /* Check whether the cell exists in the proxy */
            int n = 0;
            for (n = 0; n < p->nr_cells_in; n++)
              if (p->cells_in[n] == cj) break;
            if (n == p->nr_cells_in)
              error(
                  "Cell %d not found in the proxy but trying to construct "
                  "hydro task!",
                  cjd);
          } else if (cj->nodeID == nodeID && ci->nodeID != engine_rank) {

            /* Find the proxy for this node */
            const int proxy_id = e->proxy_ind[ci->nodeID];
            if (proxy_id < 0)
              error("No proxy exists for that foreign node %d!", ci->nodeID);

            const struct proxy *p = &e->proxies[proxy_id];

            /* Check whether the cell exists in the proxy */
            int n = 0;
            for (n = 0; n < p->nr_cells_in; n++)
              if (p->cells_in[n] == ci) break;
            if (n == p->nr_cells_in)
              error(
                  "Cell %d not found in the proxy but trying to construct "
                  "hydro task!",
                  cid);
          }
#endif /* WITH_MPI */
#endif /* SWIFT_DEBUG_CHECKS */
        }
      }
    }
  }
}

/**
 * @brief Constructs the top-level self + pair tasks for the FOF loop over
 * neighbours.
 *
 * Here we construct all the tasks for all possible neighbouring non-empty
 * local cells in the hierarchy. No dependencies are being added thus far.
 * Additional loop over neighbours can later be added by simply duplicating
 * all the tasks created by this function.
 *
 * This replaces the function in engine_maketasks but simply removes periodicity
 * (the zoom level never has periodicity), otherwise the function is identical.
 *
 * @param map_data Offset of first two indices disguised as a pointer.
 * @param num_elements Number of cells to traverse.
 * @param extra_data The #engine.
 */
void engine_make_fofloop_tasks_mapper_with_zoom(void *map_data,
                                                int num_elements,
                                                void *extra_data) {

  /* Extract the engine pointer. */
  struct engine *e = (struct engine *)extra_data;

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int *cdim = s->zoom_props->cdim;
  struct cell *cells = s->cells_top;

  /* Loop through the elements, which are just byte offsets from NULL. */
  for (int ind = 0; ind < num_elements; ind++) {

    /* Get the cell index. */
    const int cid = (size_t)(map_data) + ind;
    const int i = cid / (cdim[1] * cdim[2]);
    const int j = (cid / cdim[2]) % cdim[1];
    const int k = cid % cdim[2];

    /* Get the cell */
    struct cell *ci = &cells[cid];

    /* Skip cells without gravity particles */
    if (ci->grav.count == 0) continue;

    /* If the cells is local build a self-interaction */
    if (ci->nodeID == nodeID)
      scheduler_addtask(sched, task_type_fof_self, task_subtype_none, 0, 0, ci,
                        NULL);
    else
      continue;

    /* Now loop over all the neighbours of this cell */
    for (int ii = -1; ii < 2; ii++) {
      int iii = i + ii;
      if (iii < 0 || iii >= cdim[0]) continue;
      iii = (iii + cdim[0]) % cdim[0];
      for (int jj = -1; jj < 2; jj++) {
        int jjj = j + jj;
        if (jjj < 0 || jjj >= cdim[1]) continue;
        jjj = (jjj + cdim[1]) % cdim[1];
        for (int kk = -1; kk < 2; kk++) {
          int kkk = k + kk;
          if (kkk < 0 || kkk >= cdim[2]) continue;
          kkk = (kkk + cdim[2]) % cdim[2];

          /* Get the neighbouring cell */
          const int cjd = cell_getid(cdim, iii, jjj, kkk);
          struct cell *cj = &cells[cjd];

          /* Is that neighbour local and does it have particles ? */
          if (cid >= cjd || cj->grav.count == 0 || (ci->nodeID != cj->nodeID))
            continue;

          /* Construct the pair task */
          scheduler_addtask(sched, task_type_fof_pair, task_subtype_none, 0, 0,
                            ci, cj);
        }
      }
    }
  }
}

#endif /* WITH_ZOOM_REGION */
