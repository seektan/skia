/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrContext.h"
#include "GrLayerCache.h"
#include "GrRecordReplaceDraw.h"
#include "SkCanvasPriv.h"
#include "SkGrPixelRef.h"
#include "SkImage.h"
#include "SkRecordDraw.h"
#include "SkRecords.h"

static inline void wrap_texture(GrTexture* texture, int width, int height, SkBitmap* result) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    result->setInfo(info);
    result->setPixelRef(SkNEW_ARGS(SkGrPixelRef, (info, texture)))->unref();
}

static inline void draw_replacement_bitmap(GrCachedLayer* layer, SkCanvas* canvas) {

    SkBitmap bm;
    wrap_texture(layer->texture(),
                 !layer->isAtlased() ? layer->rect().width()  : layer->texture()->width(),
                 !layer->isAtlased() ? layer->rect().height() : layer->texture()->height(),
                 &bm);

    if (layer->isAtlased()) {
        const SkRect src = SkRect::Make(layer->rect());
        const SkRect dst = SkRect::Make(layer->srcIR());

        SkASSERT(layer->offset().isZero());

        canvas->save();
        canvas->setMatrix(SkMatrix::I());
        canvas->drawBitmapRectToRect(bm, &src, dst, layer->paint());
        canvas->restore();
    } else {
        canvas->drawSprite(bm, 
                           layer->srcIR().fLeft + layer->offset().fX,
                           layer->srcIR().fTop + layer->offset().fY,
                           layer->paint());
    }
}

// Used by GrRecordReplaceDraw. It intercepts nested drawPicture calls and
// also draws them with replaced layers.
class ReplaceDraw : public SkRecords::Draw {
public:
    ReplaceDraw(SkCanvas* canvas, GrLayerCache* layerCache,
                SkPicture const* const drawablePicts[], int drawableCount,
                const SkPicture* topLevelPicture,
                const SkPicture* picture,
                const SkMatrix& initialMatrix,
                SkPicture::AbortCallback* callback,
                const unsigned* opIndices, int numIndices)
        : INHERITED(canvas, drawablePicts, NULL, drawableCount)
        , fCanvas(canvas)
        , fLayerCache(layerCache)
        , fTopLevelPicture(topLevelPicture)
        , fPicture(picture)
        , fInitialMatrix(initialMatrix)
        , fCallback(callback)
        , fIndex(0)
        , fNumReplaced(0) {
        fOpIndexStack.append(numIndices, opIndices);
    }

    int draw() {
        const SkBBoxHierarchy* bbh = fPicture->fBBH.get();
        const SkRecord* record = fPicture->fRecord.get();
        if (NULL == record) {
            return 0;
        }

        fNumReplaced = 0;

        fOps.rewind();

        if (bbh) {
            // Draw only ops that affect pixels in the canvas's current clip.
            // The SkRecord and BBH were recorded in identity space.  This canvas
            // is not necessarily in that same space.  getClipBounds() returns us
            // this canvas' clip bounds transformed back into identity space, which
            // lets us query the BBH.
            SkRect query = { 0, 0, 0, 0 };
            (void)fCanvas->getClipBounds(&query);

            bbh->search(query, &fOps);

            for (fIndex = 0; fIndex < fOps.count(); ++fIndex) {
                if (fCallback && fCallback->abort()) {
                    return fNumReplaced;
                }

                record->visit<void>(fOps[fIndex], *this);
            }

        } else {
            for (fIndex = 0; fIndex < (int) record->count(); ++fIndex) {
                if (fCallback && fCallback->abort()) {
                    return fNumReplaced;
                }

                record->visit<void>(fIndex, *this);
            }
        }

        return fNumReplaced;
    }

    // Same as Draw for all ops except DrawPicture and SaveLayer.
    template <typename T> void operator()(const T& r) {
        this->INHERITED::operator()(r);
    }
    void operator()(const SkRecords::DrawPicture& dp) {

        int drawPictureOffset;
        if (fOps.count()) {
            drawPictureOffset = fOps[fIndex];
        } else {
            drawPictureOffset = fIndex;
        }

        fOpIndexStack.push(drawPictureOffset);

        SkAutoCanvasMatrixPaint acmp(fCanvas, &dp.matrix, dp.paint, dp.picture->cullRect());

        // Draw sub-pictures with the same replacement list but a different picture
        ReplaceDraw draw(fCanvas, fLayerCache, 
                         this->drawablePicts(), this->drawableCount(),
                         fTopLevelPicture, dp.picture, fInitialMatrix, fCallback,
                         fOpIndexStack.begin(), fOpIndexStack.count());

        fNumReplaced += draw.draw();

        fOpIndexStack.pop();
    }
    void operator()(const SkRecords::SaveLayer& sl) {

        // For a saveLayer command, check if it can be replaced by a drawBitmap
        // call and, if so, draw it and then update the current op index accordingly.
        unsigned startOffset;
        if (fOps.count()) {
            startOffset = fOps[fIndex];
        } else {
            startOffset = fIndex;
        }

        fOpIndexStack.push(startOffset);

        GrCachedLayer* layer = fLayerCache->findLayer(fTopLevelPicture->uniqueID(),
                                                      fInitialMatrix,
                                                      fOpIndexStack.begin(),
                                                      fOpIndexStack.count());

        if (layer) {
            fNumReplaced++;

            draw_replacement_bitmap(layer, fCanvas);

            if (fPicture->fBBH.get()) {
                while (fOps[fIndex] < layer->stop()) {
                    ++fIndex;
                }
                SkASSERT(fOps[fIndex] == layer->stop());
            } else {
                fIndex = layer->stop();
            }
            fOpIndexStack.pop();
            return;
        }

        // This is a fail for layer hoisting
        this->INHERITED::operator()(sl);

        fOpIndexStack.pop();
    }

private:
    SkCanvas*                 fCanvas;
    GrLayerCache*             fLayerCache;
    const SkPicture*          fTopLevelPicture;
    const SkPicture*          fPicture;
    const SkMatrix            fInitialMatrix;
    SkPicture::AbortCallback* fCallback;

    SkTDArray<unsigned>       fOps;
    int                       fIndex;
    int                       fNumReplaced;

    // The op code indices of all the enclosing drawPicture and saveLayer calls
    SkTDArray<unsigned>       fOpIndexStack;

    typedef Draw INHERITED;
};

int GrRecordReplaceDraw(const SkPicture* picture,
                        SkCanvas* canvas,
                        GrLayerCache* layerCache,
                        const SkMatrix& initialMatrix,
                        SkPicture::AbortCallback* callback) {
    SkAutoCanvasRestore saveRestore(canvas, true /*save now, restore at exit*/);

    // TODO: drawablePicts?
    ReplaceDraw draw(canvas, layerCache, NULL, 0,
                     picture, picture,
                     initialMatrix, callback, NULL, 0);
    return draw.draw();
}
