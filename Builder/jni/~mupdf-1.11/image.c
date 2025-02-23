#include "fitz-imp.h"

#define SANE_DPI 72.0f
#define INSANE_DPI 4800.0f

#define SCALABLE_IMAGE_DPI 600

#include <android/log.h>

#define DEBUG(args...) \
    __android_log_print(ANDROID_LOG_DEBUG, "MuPDF", args)

struct fz_compressed_image_s
{
	fz_image super;
	fz_pixmap *tile;
	fz_compressed_buffer *buffer;
};

struct fz_pixmap_image_s
{
	fz_image super;
	fz_pixmap *tile;
};

typedef struct fz_image_key_s fz_image_key;

struct fz_image_key_s {
	int refs;
	fz_image *image;
	int l2factor;
	fz_irect rect;
};

fz_image *
fz_keep_image(fz_context *ctx, fz_image *image)
{
	return fz_keep_key_storable(ctx, &image->key_storable);
}

fz_image *
fz_keep_image_store_key(fz_context *ctx, fz_image *image)
{
	return fz_keep_key_storable_key(ctx, &image->key_storable);
}

void
fz_drop_image_store_key(fz_context *ctx, fz_image *image)
{
	if (fz_drop_key_storable_key(ctx, &image->key_storable))
		fz_free(ctx, image);
}

static int
fz_make_hash_image_key(fz_context *ctx, fz_store_hash *hash, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;
	hash->u.pir.ptr = key->image;
	hash->u.pir.i = key->l2factor;
	hash->u.pir.r = key->rect;
	return 1;
}

static void *
fz_keep_image_key(fz_context *ctx, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;
	return fz_keep_imp(ctx, key, &key->refs);
}

static void
fz_drop_image_key(fz_context *ctx, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;
	if (fz_drop_imp(ctx, key, &key->refs))
	{
		fz_drop_image_store_key(ctx, key->image);
		fz_free(ctx, key);
	}
}

static int
fz_cmp_image_key(fz_context *ctx, void *k0_, void *k1_)
{
	fz_image_key *k0 = (fz_image_key *)k0_;
	fz_image_key *k1 = (fz_image_key *)k1_;
	return k0->image == k1->image && k0->l2factor == k1->l2factor && k0->rect.x0 == k1->rect.x0 && k0->rect.y0 == k1->rect.y0 && k0->rect.x1 == k1->rect.x1 && k0->rect.y1 == k1->rect.y1;
}

static void
fz_print_image_key(fz_context *ctx, fz_output *out, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;
	fz_write_printf(ctx, out, "(image %d x %d sf=%d) ", key->image->w, key->image->h, key->l2factor);
}

static int
fz_needs_reap_image_key(fz_context *ctx, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;

	return fz_key_storable_needs_reaping(ctx, &key->image->key_storable);
}

static const fz_store_type fz_image_store_type =
{
	fz_make_hash_image_key,
	fz_keep_image_key,
	fz_drop_image_key,
	fz_cmp_image_key,
	fz_print_image_key,
	fz_needs_reap_image_key
};

void
fz_drop_image(fz_context *ctx, fz_image *image)
{
	if (fz_drop_key_storable(ctx, &image->key_storable))
	{
		fz_drop_colorspace(ctx, image->colorspace);
		fz_drop_image(ctx, image->mask);
		fz_free(ctx, image);
	}
}

static void
fz_mask_color_key(fz_pixmap *pix, int n, const int *colorkey)
{
	unsigned char *p = pix->samples;
	int w;
	int k, t;
	int h = pix->h;
	int stride = pix->stride - pix->w * pix->n;
	if (pix->w == 0)
		return;
	while (h--)
	{
		w = pix->w;
		do
		{
			t = 1;
			for (k = 0; k < n; k++)
				if (p[k] < colorkey[k * 2] || p[k] > colorkey[k * 2 + 1])
					t = 0;
			if (t)
				for (k = 0; k < pix->n; k++)
					p[k] = 0;
			p += pix->n;
		}
		while (--w);
		p += stride;
	}
}

static void
fz_unblend_masked_tile(fz_context *ctx, fz_pixmap *tile, fz_image *image)
{
	fz_pixmap *mask = fz_get_pixmap_from_image(ctx, image->mask, NULL, NULL, NULL, NULL);
	unsigned char *s = mask->samples;
	unsigned char *d = tile->samples;
	int n = tile->n;
	int k;
	int sstride = mask->stride - mask->w * mask->n;
	int dstride = tile->stride - tile->w * tile->n;
	int h = mask->h;

	if (tile->w != mask->w || tile->h != mask->h)
	{
		fz_warn(ctx, "mask must be of same size as image for /Matte");
		fz_drop_pixmap(ctx, mask);
		return;
	}

	if (mask->w != 0)
	{
		while (h--)
		{
			int w = mask->w;
			do
			{
				if (*s == 0)
					for (k = 0; k < image->n; k++)
						d[k] = image->colorkey[k];
				else
					for (k = 0; k < image->n; k++)
						d[k] = fz_clampi(image->colorkey[k] + (d[k] - image->colorkey[k]) * 255 / *s, 0, 255);
				s++;
				d += n;
			}
			while (--w);
			s += sstride;
			d += dstride;
		}
	}

	fz_drop_pixmap(ctx, mask);
}

fz_pixmap *
fz_decomp_image_from_stream(fz_context *ctx, fz_stream *stm, fz_compressed_image *cimg, fz_irect *subarea, int indexed, int l2factor)
{
	fz_image *image = &cimg->super;
	fz_pixmap *tile = NULL;
	size_t stride, len, i;
	unsigned char *samples = NULL;
	int f = 1<<l2factor;
	int w = image->w;
	int h = image->h;

	if (subarea)
	{
		int bpp = image->bpc * image->n;
		int mask;
		switch (bpp)
		{
		case 1: mask = 8*f; break;
		case 2: mask = 4*f; break;
		case 4: mask = 2*f; break;
		default: mask = (bpp & 7) == 0 ? f : 0; break;
		}
		if (mask != 0)
		{
			subarea->x0 &= ~(mask - 1);
			subarea->x1 = (subarea->x1 + mask - 1) & ~(mask - 1);
		}
		else
		{
			/* Awkward case - mask cannot be a power of 2. */
			mask = bpp*f;
			switch (bpp)
			{
			case 3:
			case 5:
			case 7:
			case 9:
			case 11:
			case 13:
			case 15:
			default:
				mask *= 8;
				break;
			case 6:
			case 10:
			case 14:
				mask *= 4;
				break;
			case 12:
				mask *= 2;
				break;
			}
			subarea->x0 = (subarea->x0 / mask) * mask;
			subarea->x1 = ((subarea->x1 + mask - 1) / mask) * mask;
		}
		subarea->y0 &= ~(f - 1);
		if (subarea->x1 > image->w)
			subarea->x1 = image->w;
		subarea->y1 = (subarea->y1 + f - 1) & ~(f - 1);
		if (subarea->y1 > image->h)
			subarea->y1 = image->h;
		w = (subarea->x1 - subarea->x0);
		h = (subarea->y1 - subarea->y0);
	}
	w = (w + f - 1) >> l2factor;
	h = (h + f - 1) >> l2factor;

	fz_var(tile);
	fz_var(samples);

	fz_try(ctx)
	{
		int alpha = (image->colorspace == NULL);
		if (image->use_colorkey)
			alpha = 1;
		tile = fz_new_pixmap(ctx, image->colorspace, w, h, alpha);
		tile->interpolate = image->interpolate;

		stride = (w * image->n * image->bpc + 7) / 8;

		samples = fz_malloc_array(ctx, h, stride);

		if (subarea)
		{
			int hh;
			unsigned char *s = samples;
			int stream_w = (image->w + f - 1)>>l2factor;
			size_t stream_stride = (stream_w * image->n * image->bpc + 7) / 8;
			int l_margin = subarea->x0 >> l2factor;
			int t_margin = subarea->y0 >> l2factor;
			int r_margin = (image->w + f - 1 - subarea->x1) >> l2factor;
			int b_margin = (image->h + f - 1 - subarea->y1) >> l2factor;
			int l_skip = (l_margin * image->n * image->bpc)/8;
			int r_skip = (r_margin * image->n * image->bpc + 7)/8;
			size_t t_skip = t_margin * stream_stride + l_skip;
			size_t b_skip = b_margin * stream_stride + r_skip;
			size_t l = fz_skip(ctx, stm, t_skip);
			len = 0;
			if (l == t_skip)
			{
				hh = h;
				do
				{
					l = fz_read(ctx, stm, s, stride);
					s += l;
					len += l;
					if (l < stride)
						break;
					if (--hh == 0)
						break;
					l = fz_skip(ctx, stm, r_skip + l_skip);
					if (l < (size_t)(r_skip + l_skip))
						break;
				}
				while (1);
				(void)fz_skip(ctx, stm, r_skip + b_skip);
			}
		}
		else
		{
			len = fz_read(ctx, stm, samples, h * stride);
		}

		/* Pad truncated images */
		if (len < stride * h)
		{
			fz_warn(ctx, "padding truncated image");
			memset(samples + len, 0, stride * h - len);
		}

		/* Invert 1-bit image masks */
		if (image->imagemask)
		{
			/* 0=opaque and 1=transparent so we need to invert */
			unsigned char *p = samples;
			len = h * stride;
			for (i = 0; i < len; i++)
				p[i] = ~p[i];
		}

		fz_unpack_tile(ctx, tile, samples, image->n, image->bpc, stride, indexed);

		fz_free(ctx, samples);
		samples = NULL;

		/* color keyed transparency */
		if (image->use_colorkey && !image->mask)
			fz_mask_color_key(tile, image->n, image->colorkey);

		if (indexed)
		{
			fz_pixmap *conv;
			fz_decode_indexed_tile(ctx, tile, image->decode, (1 << image->bpc) - 1);
			conv = fz_expand_indexed_pixmap(ctx, tile, alpha);
			fz_drop_pixmap(ctx, tile);
			tile = conv;
		}
		else if (image->use_decode)
		{
			fz_decode_tile(ctx, tile, image->decode);
		}

		/* pre-blended matte color */
		if (image->use_colorkey && image->mask)
			fz_unblend_masked_tile(ctx, tile, image);
	}
	fz_always(ctx)
	{
		fz_drop_stream(ctx, stm);
	}
	fz_catch(ctx)
	{
		fz_drop_pixmap(ctx, tile);
		fz_free(ctx, samples);
		fz_rethrow(ctx);
	}

	return tile;
}

void
fz_drop_image_imp(fz_context *ctx, fz_storable *image_)
{
	fz_image *image = (fz_image *)image_;

	image->drop_image(ctx, image);
}

static void
drop_compressed_image(fz_context *ctx, fz_image *image_)
{
	fz_compressed_image *image = (fz_compressed_image *)image_;

	fz_drop_pixmap(ctx, image->tile);
	fz_drop_compressed_buffer(ctx, image->buffer);
}

static void
drop_pixmap_image(fz_context *ctx, fz_image *image_)
{
	fz_pixmap_image *image = (fz_pixmap_image *)image_;

	fz_drop_pixmap(ctx, image->tile);
}

static fz_pixmap *
compressed_image_get_pixmap(fz_context *ctx, fz_image *image_, fz_irect *subarea, int w, int h, int *l2factor)
{

	DEBUG("fz_pixmap compressed_image_get_pixmap");
	fz_compressed_image *image = (fz_compressed_image *)image_;
	int native_l2factor;
	fz_stream *stm;
	int indexed;
	fz_pixmap *tile;
	int can_sub = 0;

	/* We need to make a new one. */
	/* First check for ones that we can't decode using streams */
	switch (image->buffer->params.type)
	{
	case FZ_IMAGE_PNG:
		tile = fz_load_png(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_GIF:
		tile = fz_load_gif(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_BMP:
		tile = fz_load_bmp(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_WEBP:
		tile = fz_load_webp(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_TIFF:
		tile = fz_load_tiff(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_PNM:
		tile = fz_load_pnm(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_JXR:
		tile = fz_load_jxr(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_JPX:
		tile = fz_load_jpx(ctx, image->buffer->buffer->data, image->buffer->buffer->len, NULL);
		break;
	case FZ_IMAGE_JPEG:
		/* Scan JPEG stream and patch missing height values in header */
		{
			unsigned char *s = image->buffer->buffer->data;
			unsigned char *e = s + image->buffer->buffer->len;
			unsigned char *d;
			for (d = s + 2; s < d && d < e - 9 && d[0] == 0xFF; d += (d[2] << 8 | d[3]) + 2)
			{
				if (d[1] < 0xC0 || (0xC3 < d[1] && d[1] < 0xC9) || 0xCB < d[1])
					continue;
				if ((d[5] == 0 && d[6] == 0) || ((d[5] << 8) | d[6]) > image->super.h)
				{
					d[5] = (image->super.h >> 8) & 0xFF;
					d[6] = image->super.h & 0xFF;
				}
			}
		}
		/* fall through */

	default:
		native_l2factor = l2factor ? *l2factor : 0;
		stm = fz_open_image_decomp_stream_from_buffer(ctx, image->buffer, l2factor);
		if (l2factor)
			native_l2factor -= *l2factor;

		indexed = fz_colorspace_is_indexed(ctx, image->super.colorspace);
		can_sub = 1;
		tile = fz_decomp_image_from_stream(ctx, stm, image, subarea, indexed, native_l2factor);

		/* CMYK JPEGs in XPS documents have to be inverted */
		if (image->super.invert_cmyk_jpeg &&
			image->buffer->params.type == FZ_IMAGE_JPEG &&
			image->super.colorspace == fz_device_cmyk(ctx) &&
			image->buffer->params.u.jpeg.color_transform)
		{
			fz_invert_pixmap(ctx, tile);
		}

		break;
	}

	if (can_sub == 0 && subarea != NULL)
	{
		subarea->x0 = 0;
		subarea->y0 = 0;
		subarea->x1 = image->super.w;
		subarea->y1 = image->super.h;
	}

	return tile;
}

static fz_pixmap *
pixmap_image_get_pixmap(fz_context *ctx, fz_image *image_, fz_irect *subarea, int w, int h, int *l2factor)
{
	fz_pixmap_image *image = (fz_pixmap_image *)image_;

	/* 'Simple' images created direct from pixmaps will have no buffer
	 * of compressed data. We cannot do any better than just returning
	 * a pointer to the original 'tile'.
	 */
	return fz_keep_pixmap(ctx, image->tile); /* That's all we can give you! */
}

static void
update_ctm_for_subarea(fz_matrix *ctm, const fz_irect *subarea, int w, int h)
{
	fz_matrix m;

	if (subarea->x0 == 0 && subarea->y0 == 0 && subarea->x1 == w && subarea->y1 == h)
		return;

	m.a = (subarea->x1 - subarea->x0) / (float)w;
	m.b = 0;
	m.c = 0;
	m.d = (subarea->y1 - subarea->y0) / (float)h;
	m.e = subarea->x0 / (float)w;
	m.f = subarea->y0 / (float)h;
	fz_concat(ctm, &m, ctm);
}

void fz_default_image_decode(void *arg, int w, int h, int l2factor, fz_irect *subarea)
{
	(void)arg;

	if ((subarea->x1-subarea->x0)*(subarea->y1-subarea->y0) >= (w*h/10)*9)
	{
		/* Either no subarea specified, or a subarea 90% or more of the
		 * whole area specified. Use the whole image. */
		subarea->x0 = 0;
		subarea->y0 = 0;
		subarea->x1 = w;
		subarea->y1 = h;
	}
	else
	{
		/* Clip to the edges if they are within 1% */
		if (subarea->x0 <= w/100)
			subarea->x0 = 0;
		if (subarea->y0 <= h/100)
			subarea->y0 = 0;
		if (subarea->x1 >= w*99/100)
			subarea->x1 = w;
		if (subarea->y1 >= h*99/100)
			subarea->y1 = h;
	}
}

fz_pixmap *
fz_get_pixmap_from_image(fz_context *ctx, fz_image *image, const fz_irect *subarea, fz_matrix *ctm, int *dw, int *dh)
{
	fz_pixmap *tile;
	int l2factor, l2factor_remaining;
	fz_image_key key;
	fz_image_key *keyp;
	int w;
	int h;

	if (!image)
		return NULL;

	/* Figure out the extent. */
	if (ctm)
	{
		w = sqrtf(ctm->a * ctm->a + ctm->b * ctm->b);
		h = sqrtf(ctm->c * ctm->c + ctm->d * ctm->d);
	}
	else
	{
		w = image->w;
		h = image->h;
	}

	if (image->scalable)
	{
		/* If the image is scalable, we always want to re-render and never cache. */
		fz_irect subarea_copy;
		if (subarea)
			subarea_copy = *subarea;
		l2factor_remaining = 0;
		if (dw) *dw = w;
		if (dh) *dh = h;
		return image->get_pixmap(ctx, image, subarea ? &subarea_copy : NULL, image->w, image->h, &l2factor_remaining);
	}

	/* Clamp requested image size, since we never want to magnify images here. */
	if (w > image->w)
		w = image->w;
	if (h > image->h)
		h = image->h;

	if (image->decoded)
	{
		/* If the image is already decoded, then we can't offer a subarea,
		 * or l2factor, and we don't want to cache. */
		l2factor_remaining = 0;
		if (dw) *dw = w;
		if (dh) *dh = h;
		return image->get_pixmap(ctx, image, NULL, image->w, image->h, &l2factor_remaining);
	}

	/* What is our ideal factor? We search for the largest factor where
	 * we can subdivide and stay larger than the required size. We add
	 * a fudge factor of +2 here to allow for the possibility of
	 * expansion due to grid fitting. */
	if (w == 0 || h == 0)
		l2factor = 0;
	else
		for (l2factor=0; image->w>>(l2factor+1) >= w+2 && image->h>>(l2factor+1) >= h+2 && l2factor < 6; l2factor++);

	/* Now figure out if we want to decode just a subarea */
	if (subarea == NULL)
	{
		key.rect.x0 = 0;
		key.rect.y0 = 0;
		key.rect.x1 = image->w;
		key.rect.y1 = image->h;
	}
	else
	{
		key.rect = *subarea;
		ctx->tuning->image_decode(ctx->tuning->image_decode_arg, image->w, image->h, l2factor, &key.rect);
	}

	/* Based on that subarea, recalculate the extents */
	if (ctm)
	{
		float frac_w = (key.rect.x1 - key.rect.x0) / (float)image->w;
		float frac_h = (key.rect.y1 - key.rect.y0) / (float)image->h;
		float a = ctm->a * frac_w;
		float b = ctm->b * frac_h;
		float c = ctm->c * frac_w;
		float d = ctm->d * frac_h;

		w = sqrtf(a * a + b * b);
		h = sqrtf(c * c + d * d);
	}
	else
	{
		w = image->w;
		h = image->h;
	}

	/* Return the true sizes to the caller */
	if (dw)
		*dw = w;
	if (dh)
		*dh = h;
	if (w > image->w)
		w = image->w;
	if (h > image->h)
		h = image->h;

	if (w == 0 || h == 0)
		l2factor = 0;

	/* Can we find any suitable tiles in the cache? */
	key.refs = 1;
	key.image = image;
	key.l2factor = l2factor;
	do
	{
		tile = fz_find_item(ctx, fz_drop_pixmap_imp, &key, &fz_image_store_type);
		if (tile)
		{
			update_ctm_for_subarea(ctm, &key.rect, image->w, image->h);
			return tile;
		}
		key.l2factor--;
	}
	while (key.l2factor >= 0);

	/* We'll have to decode the image; request the correct amount of
	 * downscaling. */
	l2factor_remaining = l2factor;
	tile = image->get_pixmap(ctx, image, &key.rect, w, h, &l2factor_remaining);

	/* Update the ctm to allow for subareas. */
	update_ctm_for_subarea(ctm, &key.rect, image->w, image->h);

	/* l2factor_remaining is updated to the amount of subscaling left to do */
	assert(l2factor_remaining >= 0 && l2factor_remaining <= 6);
	if (l2factor_remaining)
	{
		fz_subsample_pixmap(ctx, tile, l2factor_remaining);
	}

	/* Now we try to cache the pixmap. Any failure here will just result
	 * in us not caching. */
	fz_var(keyp);
	fz_try(ctx)
	{
		fz_pixmap *existing_tile;

		keyp = fz_malloc_struct(ctx, fz_image_key);
		keyp->refs = 1;
		keyp->image = fz_keep_image_store_key(ctx, image);
		keyp->l2factor = l2factor;
		keyp->rect = key.rect;
		existing_tile = fz_store_item(ctx, keyp, tile, fz_pixmap_size(ctx, tile), &fz_image_store_type);
		if (existing_tile)
		{
			/* We already have a tile. This must have been produced by a
			 * racing thread. We'll throw away ours and use that one. */
			fz_drop_pixmap(ctx, tile);
			tile = existing_tile;
		}
	}
	fz_always(ctx)
	{
		fz_drop_image_key(ctx, keyp);
	}
	fz_catch(ctx)
	{
		/* Do nothing */
	}

	return tile;
}

static size_t
pixmap_image_get_size(fz_context *ctx, fz_image *image)
{
	fz_pixmap_image *im = (fz_pixmap_image *)image;

	if (image == NULL)
		return 0;

	return sizeof(fz_pixmap_image) + fz_pixmap_size(ctx, im->tile);
}

size_t fz_image_size(fz_context *ctx, fz_image *im)
{
	if (im == NULL)
		return 0;

	return im->get_size(ctx, im);
}

fz_image *
fz_new_image_from_pixmap(fz_context *ctx, fz_pixmap *pixmap, fz_image *mask)
{
	fz_pixmap_image *image;

	image = fz_new_derived_image(ctx, pixmap->w, pixmap->h, 8, pixmap->colorspace,
				pixmap->xres, pixmap->yres, 0, 0,
				NULL, NULL, mask, fz_pixmap_image,
				pixmap_image_get_pixmap,
				pixmap_image_get_size,
				drop_pixmap_image);
	image->tile = fz_keep_pixmap(ctx, pixmap);
	image->super.decoded = 1;

	return &image->super;
}

fz_image *
fz_new_image_of_size(fz_context *ctx, int w, int h, int bpc, fz_colorspace *colorspace,
		int xres, int yres, int interpolate, int imagemask, float *decode,
		int *colorkey, fz_image *mask, int size, fz_image_get_pixmap_fn *get,
		fz_image_get_size_fn *get_size, fz_drop_image_fn *drop)
{
	fz_image *image;
	int i;

	assert(mask == NULL || mask->mask == NULL);
	assert(size >= sizeof(fz_image));

	image = Memento_label(fz_calloc(ctx, 1, size), "fz_image");
	FZ_INIT_KEY_STORABLE(image, 1, fz_drop_image_imp);
	image->drop_image = drop;
	image->get_pixmap = get;
	image->get_size = get_size;
	image->w = w;
	image->h = h;
	image->xres = xres;
	image->yres = yres;
	image->bpc = bpc;
	image->n = (colorspace ? fz_colorspace_n(ctx, colorspace) : 1);
	image->colorspace = fz_keep_colorspace(ctx, colorspace);
	image->invert_cmyk_jpeg = 1;
	image->interpolate = interpolate;
	image->imagemask = imagemask;
	image->use_colorkey = (colorkey != NULL);
	if (colorkey)
		memcpy(image->colorkey, colorkey, sizeof(int)*image->n*2);
	image->use_decode = 0;
	if (decode)
	{
		memcpy(image->decode, decode, sizeof(float)*image->n*2);
	}
	else
	{
		float maxval = fz_colorspace_is_indexed(ctx, colorspace) ? (1 << bpc) - 1 : 1;
		for (i = 0; i < image->n; i++)
		{
			image->decode[2*i] = 0;
			image->decode[2*i+1] = maxval;
		}
	}
	for (i = 0; i < image->n; i++)
	{
		if (image->decode[i * 2] * 255 != 0 || image->decode[i * 2 + 1] * 255 != 255)
			break;
	}
	if (i != image->n)
		image->use_decode = 1;
	image->mask = fz_keep_image(ctx, mask);

	return image;
}

static size_t
compressed_image_get_size(fz_context *ctx, fz_image *image)
{
	fz_compressed_image *im = (fz_compressed_image *)image;

	if (image == NULL)
		return 0;

	return sizeof(fz_pixmap_image) + fz_pixmap_size(ctx, im->tile) + (im->buffer && im->buffer->buffer ? im->buffer->buffer->cap : 0);
}

fz_image *
fz_new_image_from_compressed_buffer(fz_context *ctx, int w, int h,
	int bpc, fz_colorspace *colorspace,
	int xres, int yres, int interpolate, int imagemask, float *decode,
	int *colorkey, fz_compressed_buffer *buffer, fz_image *mask)
{
	fz_compressed_image *image;

	fz_try(ctx)
	{
		image = fz_new_derived_image(ctx, w, h, bpc,
					colorspace, xres, yres,
					interpolate, imagemask, decode,
					colorkey, mask, fz_compressed_image,
					compressed_image_get_pixmap,
					compressed_image_get_size,
					drop_compressed_image);
		image->buffer = buffer;
	}
	fz_catch(ctx)
	{
		fz_drop_compressed_buffer(ctx, buffer);
		fz_rethrow(ctx);
	}

	return &image->super;
}

fz_compressed_buffer *fz_compressed_image_buffer(fz_context *ctx, fz_image *image)
{
	if (image == NULL || image->get_pixmap != compressed_image_get_pixmap)
		return NULL;
	return ((fz_compressed_image *)image)->buffer;
}

void fz_set_compressed_image_buffer(fz_context *ctx, fz_compressed_image *image, fz_compressed_buffer *buf)
{
	assert(image != NULL && image->super.get_pixmap == compressed_image_get_pixmap);
	((fz_compressed_image *)image)->buffer = buf;
}

fz_pixmap *fz_compressed_image_tile(fz_context *ctx, fz_compressed_image *image)
{
	if (image == NULL || image->super.get_pixmap != compressed_image_get_pixmap)
		return NULL;
	return ((fz_compressed_image *)image)->tile;
}

void fz_set_compressed_image_tile(fz_context *ctx, fz_compressed_image *image, fz_pixmap *pix)
{
	assert(image != NULL && image->super.get_pixmap == compressed_image_get_pixmap);
	((fz_compressed_image *)image)->tile = pix;
}

fz_pixmap *fz_pixmap_image_tile(fz_context *ctx, fz_pixmap_image *image)
{
	if (image == NULL || image->super.get_pixmap != pixmap_image_get_pixmap)
		return NULL;
	return ((fz_pixmap_image *)image)->tile;
}

void fz_set_pixmap_image_tile(fz_context *ctx, fz_pixmap_image *image, fz_pixmap *pix)
{
	assert(image != NULL && image->super.get_pixmap == pixmap_image_get_pixmap);
	((fz_pixmap_image *)image)->tile = pix;
}

fz_image *
fz_new_image_from_buffer(fz_context *ctx, fz_buffer *buffer)
{


	fz_compressed_buffer *bc;
	int w, h, xres, yres;
	fz_colorspace *cspace = NULL;
	size_t len = buffer->len;
	unsigned char *buf = buffer->data;
	fz_image *image;
	int type;

	if (len < 8)
		fz_throw(ctx, FZ_ERROR_GENERIC, "unknown image file format");

	fz_var(cspace);

	fz_try(ctx)
	{

		DEBUG("fz_new_image_from_buffer %c %c %c",buf[0],buf[1],buf[2]);

		if (buf[0] == 'P' && buf[1] >= '1' && buf[1] <= '7')
		{
			DEBUG("fz_new_image_from_buffer FZ_IMAGE_PNM");
			type = FZ_IMAGE_PNM;
			fz_load_pnm_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 0xff && buf[1] == 0x4f)
		{
			DEBUG("fz_new_image_from_buffer FZ_IMAGE_JPX");
			type = FZ_IMAGE_JPX;
			fz_load_jpx_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x0c && buf[4] == 0x6a && buf[5] == 0x50 && buf[6] == 0x20 && buf[7] == 0x20)
		{
			DEBUG("fz_new_image_from_buffer FZ_IMAGE_JPX");
			type = FZ_IMAGE_JPX;
			fz_load_jpx_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 0xff && buf[1] == 0xd8)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_JPEG");
			type = FZ_IMAGE_JPEG;
			fz_load_jpeg_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (memcmp(buf, "\211PNG\r\n\032\n", 8) == 0)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_PNG");
			type = FZ_IMAGE_PNG;
			fz_load_png_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 'I' && buf[1] == 'I' && buf[2] == 0xBC)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_JXR");
			type = FZ_IMAGE_JXR;
			fz_load_jxr_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 'I' && buf[1] == 'I' && buf[2] == 42 && buf[3] == 0)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_TIFF");
			type = FZ_IMAGE_TIFF;
			fz_load_tiff_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (buf[0] == 'M' && buf[1] == 'M' && buf[2] == 0 && buf[3] == 42)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_TIFF");
			type = FZ_IMAGE_TIFF;
			fz_load_tiff_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (memcmp(buf, "GIF", 3) == 0)
		{
			DEBUG("fz_new_image_from_buffer FZ_IMAGE_GIF");
			type = FZ_IMAGE_GIF;
			fz_load_gif_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (memcmp(buf, "BM", 2) == 0)
		{
		DEBUG("fz_new_image_from_buffer FZ_IMAGE_BMP");
			type = FZ_IMAGE_BMP;
			fz_load_bmp_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}else if (memcmp(buf, "RIFF", 4) == 0)
		{
			DEBUG("fz_new_image_from_buffer WEBP");
			type = FZ_IMAGE_WEBP;
			fz_load_webp_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else{
			DEBUG("fz_new_image_from_buffer unknown");
			fz_throw(ctx, FZ_ERROR_GENERIC, "unknown image file format");
			}


		bc = fz_malloc_struct(ctx, fz_compressed_buffer);
		bc->buffer = fz_keep_buffer(ctx, buffer);
		bc->params.type = type;
		if (type == FZ_IMAGE_JPEG)
			bc->params.u.jpeg.color_transform = -1;
		image = fz_new_image_from_compressed_buffer(ctx, w, h, 8, cspace, xres, yres, 0, 0, NULL, NULL, bc, NULL);
	}
	fz_always(ctx)
		fz_drop_colorspace(ctx, cspace);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return image;
}

fz_image *
fz_new_image_from_file(fz_context *ctx, const char *path)
{
	fz_buffer *buffer;
	fz_image *image;

	buffer = fz_read_file(ctx, path);
	fz_try(ctx)
		image = fz_new_image_from_buffer(ctx, buffer);
	fz_always(ctx)
		fz_drop_buffer(ctx, buffer);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return image;
}

void
fz_image_resolution(fz_image *image, int *xres, int *yres)
{
	*xres = image->xres;
	*yres = image->yres;
	if (*xres < 0 || *yres < 0 || (*xres == 0 && *yres == 0))
	{
		/* If neither xres or yres is sane, pick a sane value */
		*xres = SANE_DPI; *yres = SANE_DPI;
	}
	else if (*xres == 0)
	{
		*xres = *yres;
	}
	else if (*yres == 0)
	{
		*yres = *xres;
	}

	/* Scale xres and yres up until we get believable values */
	if (*xres < SANE_DPI || *yres < SANE_DPI || *xres > INSANE_DPI || *yres > INSANE_DPI)
	{
		if (*xres == *yres)
		{
			*xres = SANE_DPI;
			*yres = SANE_DPI;
		}
		else if (*xres < *yres)
		{
			*yres = *yres * SANE_DPI / *xres;
			*xres = SANE_DPI;
		}
		else
		{
			*xres = *xres * SANE_DPI / *yres;
			*yres = SANE_DPI;
		}
	}
}

typedef struct fz_display_list_image_s
{
	fz_image super;
	fz_matrix transform;
	fz_display_list *list;
} fz_display_list_image;

static fz_pixmap *
display_list_image_get_pixmap(fz_context *ctx, fz_image *image_, fz_irect *subarea, int w, int h, int *l2factor)
{
	fz_display_list_image *image = (fz_display_list_image *)image_;
	fz_matrix ctm;
	fz_device *dev;
	fz_pixmap *pix;

	if (subarea)
	{
		/* So, the whole image should be scaled to w * h, but we only want the
		 * given subarea of it. */
		int l = (subarea->x0 * w) / image->super.w;
		int t = (subarea->y0 * h) / image->super.h;
		int r = (subarea->x1 * w + image->super.w - 1) / image->super.w;
		int b = (subarea->y1 * h + image->super.h - 1) / image->super.h;

		pix = fz_new_pixmap(ctx, image->super.colorspace, r-l, b-t, 0);
		pix->x = l;
		pix->y = t;
	}
	else
	{
		pix = fz_new_pixmap(ctx, image->super.colorspace, w, h, 0);
	}

	/* If we render the display list into pix with the image matrix, we'll get a unit
	 * square result. Therefore scale by w, h. */
	ctm = image->transform;
	fz_pre_scale(&ctm, w, h);

	fz_clear_pixmap(ctx, pix); /* clear to transparent */
	dev = fz_new_draw_device(ctx, &ctm, pix);
	fz_run_display_list(ctx, image->list, dev, &fz_identity, NULL, NULL);
	fz_close_device(ctx, dev);
	fz_drop_device(ctx, dev);

	/* Never do more subsampling, cos we've already given them the right size */
	if (l2factor)
		*l2factor = 0;

	return pix;
}

static void drop_display_list_image(fz_context *ctx, fz_image *image_)
{
	fz_display_list_image *image = (fz_display_list_image *)image_;

	if (image == NULL)
		return;
	fz_drop_display_list(ctx, image->list);
}

static size_t
display_list_image_get_size(fz_context *ctx, fz_image *image_)
{
	fz_display_list_image *image = (fz_display_list_image *)image_;

	if (image == NULL)
		return 0;

	return sizeof(fz_display_list_image) + 4096; /* FIXME */
}

fz_image *fz_new_image_from_display_list(fz_context *ctx, float w, float h, fz_display_list *list)
{
	fz_display_list_image *image;
	int iw, ih;

	iw = w * SCALABLE_IMAGE_DPI / 72;
	ih = h * SCALABLE_IMAGE_DPI / 72;

	image = fz_new_derived_image(ctx, iw, ih, 8, fz_device_rgb(ctx),
				SCALABLE_IMAGE_DPI, SCALABLE_IMAGE_DPI, 0, 0,
				NULL, NULL, NULL, fz_display_list_image,
				display_list_image_get_pixmap,
				display_list_image_get_size,
				drop_display_list_image);
	image->super.scalable = 1;
	fz_scale(&image->transform, 1 / w, 1 / h);
	image->list = fz_keep_display_list(ctx, list);

	return &image->super;
}
