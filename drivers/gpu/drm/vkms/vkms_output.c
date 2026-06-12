// SPDX-License-Identifier: GPL-2.0+

#include "vkms_config.h"
#include "vkms_connector.h"
#include "vkms_drv.h"
#include <drm/drm_managed.h>

int vkms_output_init(struct vkms_device *vkmsdev)
{
	struct drm_device *dev = &vkmsdev->drm;
	struct vkms_config_plane *plane_cfg;
	struct vkms_config_crtc *crtc_cfg;
	struct vkms_config_encoder *encoder_cfg;
	struct vkms_config_connector *connector_cfg;
	int ret;
	int writeback;

	/*
	 * Materialize the validated vkms_config graph in dependency order.
	 * Each config node keeps a temporary back-pointer to the managed DRM
	 * object created for it, and the later passes turn those pointers into
	 * the routing masks and connector attachments used by the DRM core.
	 */
	if (!vkms_config_is_valid(vkmsdev->config))
		return -EINVAL;

	/*
	 * Plane stage:
	 * vkms_plane_init() allocates a managed struct vkms_plane, installs the
	 * plane and helper callbacks, and exposes the rotation plus YUV color
	 * properties that the compositor will consume later.
	 */
	vkms_config_for_each_plane(vkmsdev->config, plane_cfg) {
		enum drm_plane_type type;

		type = vkms_config_plane_get_type(plane_cfg);

		plane_cfg->plane = vkms_plane_init(vkmsdev, type);
		if (IS_ERR(plane_cfg->plane)) {
			DRM_DEV_ERROR(dev->dev, "Failed to init vkms plane\n");
			return PTR_ERR(plane_cfg->plane);
		}
	}

	/*
	 * Plane objects now exist, but they are still only reachable through the
	 * temporary config back-pointers:
	 *
	 *   vkmsdev->config
	 *      |
	 *      +-- plane_cfg[type=PRIMARY] ----> struct vkms_plane
	 *      |                                   `-- base: struct drm_plane
	 *      +-- plane_cfg[type=OVERLAY] ----> struct vkms_plane (optional)
	 *      `-- plane_cfg[type=CURSOR]  ----> struct vkms_plane (optional)
	 */

	/*
	 * CRTC stage:
	 * the config identifies which primary plane is mandatory and which cursor
	 * plane is optional for each CRTC. vkms_crtc_init() then allocates a
	 * struct vkms_output around an embedded drm_crtc, adds the CRTC helper
	 * hooks, enables gamma/color-management state, and prepares the locks and
	 * ordered workqueue used by the composer. If requested, the writeback
	 * helper extends that same vkms_output with an embedded writeback encoder
	 * and connector.
	 */
	vkms_config_for_each_crtc(vkmsdev->config, crtc_cfg) {
		struct vkms_config_plane *primary, *cursor;

		primary = vkms_config_crtc_primary_plane(vkmsdev->config, crtc_cfg);
		cursor = vkms_config_crtc_cursor_plane(vkmsdev->config, crtc_cfg);

		crtc_cfg->crtc = vkms_crtc_init(dev, &primary->plane->base,
						cursor ? &cursor->plane->base : NULL);
		if (IS_ERR(crtc_cfg->crtc)) {
			DRM_ERROR("Failed to allocate CRTC\n");
			return PTR_ERR(crtc_cfg->crtc);
		}

		/* Initialize the writeback component */
		if (vkms_config_crtc_get_writeback(crtc_cfg)) {
			writeback = vkms_enable_writeback_connector(vkmsdev, crtc_cfg->crtc);
			if (writeback)
				DRM_ERROR("Failed to init writeback connector\n");
		}
	}

	/*
	 * Each CRTC config node now points at the vkms_output that embeds the DRM
	 * objects used by the rest of the pipeline:
	 *
	 *   primary->plane->base ----\
	 *                             +--> vkms_output.crtc
	 *   cursor->plane->base  ----/        ^
	 *        (optional)                     |
	 *                                       |
	 *   crtc_cfg -------------------------->+-- struct vkms_output
	 *                                            +-- crtc
	 *                                            +-- composer_workq
	 *                                            +-- lock/composer_lock
	 *                                            `-- wb_encoder +
	 *                                                wb_connector (optional)
	 */

	/*
	 * Plane routing stage:
	 * copy the config-level "possible CRTC" xarray into the DRM bitmask that
	 * atomic helpers consult when validating plane placement.
	 */
	vkms_config_for_each_plane(vkmsdev->config, plane_cfg) {
		struct vkms_config_crtc *possible_crtc;
		unsigned long idx = 0;

		vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			plane_cfg->plane->base.possible_crtcs |=
				drm_crtc_mask(&possible_crtc->crtc->crtc);
		}
	}

	/*
	 * Every plane now carries both views of the same topology:
	 *
	 *   plane_cfg->possible_crtcs[idx] ----> possible_crtc->crtc
	 *                                         `-- crtc: struct vkms_output
	 *                                             `-- crtc: struct drm_crtc
	 *
	 *   plane_cfg->plane->base.possible_crtcs = OR(drm_crtc_mask(...))
	 */

	/*
	 * Encoder stage:
	 * each encoder is allocated as a managed DRM virtual encoder, then its
	 * config-level CRTC fan-out is copied into encoder->possible_crtcs so the
	 * connector stage can bind only legal display routes.
	 */
	vkms_config_for_each_encoder(vkmsdev->config, encoder_cfg) {
		struct vkms_config_crtc *possible_crtc;
		unsigned long idx = 0;

		encoder_cfg->encoder = drmm_kzalloc(dev, sizeof(*encoder_cfg->encoder), GFP_KERNEL);
		if (!encoder_cfg->encoder) {
			DRM_ERROR("Failed to allocate encoder\n");
			return -ENOMEM;
		}
		ret = drmm_encoder_init(dev, encoder_cfg->encoder, NULL,
					DRM_MODE_ENCODER_VIRTUAL, NULL);
		if (ret) {
			DRM_ERROR("Failed to init encoder\n");
			return ret;
		}

		vkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
			encoder_cfg->encoder->possible_crtcs |=
				drm_crtc_mask(&possible_crtc->crtc->crtc);
		}
	}

	/*
	 * Encoders are now live and point back at the CRTCs they may drive:
	 *
	 *   encoder_cfg -----------------------> struct drm_encoder
	 *                                         `-- possible_crtcs bitmask
	 *
	 *   encoder_cfg->possible_crtcs[idx] --> possible_crtc->crtc->crtc
	 */

	/*
	 * Connector stage:
	 * vkms_connector_init() allocates a managed struct vkms_connector,
	 * registers a DRM virtual connector, and adds helpers that advertise
	 * no-EDID modes and pick the first attached encoder. This loop then
	 * converts each config edge into drm_connector_attach_encoder().
	 */
	vkms_config_for_each_connector(vkmsdev->config, connector_cfg) {
		struct vkms_config_encoder *possible_encoder;
		unsigned long idx = 0;

		connector_cfg->connector = vkms_connector_init(vkmsdev);
		if (IS_ERR(connector_cfg->connector)) {
			DRM_ERROR("Failed to init connector\n");
			return PTR_ERR(connector_cfg->connector);
		}

		vkms_config_connector_for_each_possible_encoder(connector_cfg,
								idx,
								possible_encoder) {
			ret = drm_connector_attach_encoder(&connector_cfg->connector->base,
							   possible_encoder->encoder);
			if (ret) {
				DRM_ERROR("Failed to attach connector to encoder\n");
				return ret;
			}
		}
	}

	/*
	 * Connectors now expose both the config edge and the live DRM link:
	 *
	 *   connector_cfg --------------------> struct vkms_connector
	 *                                         `-- base: struct drm_connector
	 *
	 *   connector_cfg->possible_encoders[idx] -> struct drm_encoder
	 *   drm_connector_attach_encoder()        -> connector <-> encoder
	 */

	/*
	 * The full display graph is now connected:
	 *
	 *   struct vkms_plane(s)
	 *          |
	 *          `-- possible_crtcs ----> struct drm_crtc
	 *                                      `-- embedded in struct vkms_output
	 *                                                |
	 *                                                +-- optional writeback
	 *                                                |   encoder/connector
	 *                                                |
	 *                                                `-- struct drm_encoder(s)
	 *                                                           |
	 *                                                           `-- attached to
	 *                                                               struct
	 *                                                               vkms_connector
	 *                                                               / drm_connector
	 *
	 * drm_mode_config_reset() snapshots this graph into the DRM core's default
	 * object state before the device is registered.
	 */
	drm_mode_config_reset(dev);

	return 0;
}
