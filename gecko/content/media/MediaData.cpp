/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "MediaData.h"
#include "MediaInfo.h"
#ifdef MOZ_OMX_DECODER
#include "GrallocImages.h"
#endif
#include "VideoUtils.h"
#include "ImageContainer.h"

namespace mozilla {

using namespace mozilla::gfx;
using layers::ImageContainer;
using layers::PlanarYCbCrImage;
using layers::PlanarYCbCrData;

// Verify these values are sane. Once we've checked the frame sizes, we then
// can do less integer overflow checking.
static_assert(MAX_VIDEO_WIDTH < PlanarYCbCrImage::MAX_DIMENSION,
              "MAX_VIDEO_WIDTH is too large");
static_assert(MAX_VIDEO_HEIGHT < PlanarYCbCrImage::MAX_DIMENSION,
              "MAX_VIDEO_HEIGHT is too large");
static_assert(PlanarYCbCrImage::MAX_DIMENSION < UINT32_MAX / PlanarYCbCrImage::MAX_DIMENSION,
              "MAX_DIMENSION*MAX_DIMENSION doesn't fit in 32 bits");

void
AudioData::EnsureAudioBuffer()
{
  if (mAudioBuffer)
    return;
  mAudioBuffer = SharedBuffer::Create(mFrames*mChannels*sizeof(AudioDataValue));

  AudioDataValue* data = static_cast<AudioDataValue*>(mAudioBuffer->Data());
  for (uint32_t i = 0; i < mFrames; ++i) {
    for (uint32_t j = 0; j < mChannels; ++j) {
      data[j*mFrames + i] = mAudioData[i*mChannels + j];
    }
  }
}

static bool
ValidatePlane(const VideoData::YCbCrBuffer::Plane& aPlane)
{
  return aPlane.mWidth <= PlanarYCbCrImage::MAX_DIMENSION &&
         aPlane.mHeight <= PlanarYCbCrImage::MAX_DIMENSION &&
         aPlane.mWidth * aPlane.mHeight < MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
         aPlane.mStride > 0;
}

static bool
IsYV12Format(const VideoData::YCbCrBuffer::Plane& aYPlane,
             const VideoData::YCbCrBuffer::Plane& aCbPlane,
             const VideoData::YCbCrBuffer::Plane& aCrPlane)
{
  return
    aYPlane.mWidth % 2 == 0 &&
    aYPlane.mHeight % 2 == 0 &&
    aYPlane.mWidth / 2 == aCbPlane.mWidth &&
    aYPlane.mHeight / 2 == aCbPlane.mHeight &&
    aCbPlane.mWidth == aCrPlane.mWidth &&
    aCbPlane.mHeight == aCrPlane.mHeight;
}

VideoData::VideoData(int64_t aOffset, int64_t aTime, int64_t aDuration, int64_t aTimecode)
  : MediaData(VIDEO_FRAME, aOffset, aTime, aDuration),
    mTimecode(aTimecode),
    mDuplicate(true),
    mKeyframe(false)
{
  MOZ_COUNT_CTOR(VideoData);
  NS_ASSERTION(mDuration >= 0, "Frame must have non-negative duration.");
}

VideoData::VideoData(int64_t aOffset,
                     int64_t aTime,
                     int64_t aDuration,
                     bool aKeyframe,
                     int64_t aTimecode,
                     nsIntSize aDisplay)
  : MediaData(VIDEO_FRAME, aOffset, aTime, aDuration),
    mDisplay(aDisplay),
    mTimecode(aTimecode),
    mDuplicate(false),
    mKeyframe(aKeyframe)
{
  MOZ_COUNT_CTOR(VideoData);
  NS_ASSERTION(mDuration >= 0, "Frame must have non-negative duration.");
}

VideoData::~VideoData()
{
  MOZ_COUNT_DTOR(VideoData);
}

/* static */
VideoData* VideoData::ShallowCopyUpdateDuration(VideoData* aOther,
                                                int64_t aDuration)
{
  VideoData* v = new VideoData(aOther->mOffset,
                               aOther->mTime,
                               aDuration,
                               aOther->mKeyframe,
                               aOther->mTimecode,
                               aOther->mDisplay);
  v->mImage = aOther->mImage;
  return v;
}

VideoData* VideoData::Create(VideoInfo& aInfo,
                             ImageContainer* aContainer,
                             Image* aImage,
                             int64_t aOffset,
                             int64_t aTime,
                             int64_t aDuration,
                             const YCbCrBuffer& aBuffer,
                             bool aKeyframe,
                             int64_t aTimecode,
                             nsIntRect aPicture)
{
  if (!aImage && !aContainer) {
    // Create a dummy VideoData with no image. This gives us something to
    // send to media streams if necessary.
    nsAutoPtr<VideoData> v(new VideoData(aOffset,
                                         aTime,
                                         aDuration,
                                         aKeyframe,
                                         aTimecode,
                                         aInfo.mDisplay));
    return v.forget();
  }

  // The following situation should never happen unless there is a bug
  // in the decoder
  if (aBuffer.mPlanes[1].mWidth != aBuffer.mPlanes[2].mWidth ||
      aBuffer.mPlanes[1].mHeight != aBuffer.mPlanes[2].mHeight) {
    NS_ERROR("C planes with different sizes");
    return nullptr;
  }

  // The following situations could be triggered by invalid input
  if (aPicture.width <= 0 || aPicture.height <= 0) {
    NS_WARNING("Empty picture rect");
    return nullptr;
  }
  if (!ValidatePlane(aBuffer.mPlanes[0]) || !ValidatePlane(aBuffer.mPlanes[1]) ||
      !ValidatePlane(aBuffer.mPlanes[2])) {
    NS_WARNING("Invalid plane size");
    return nullptr;
  }

  // Ensure the picture size specified in the headers can be extracted out of
  // the frame we've been supplied without indexing out of bounds.
  CheckedUint32 xLimit = aPicture.x + CheckedUint32(aPicture.width);
  CheckedUint32 yLimit = aPicture.y + CheckedUint32(aPicture.height);
  if (!xLimit.isValid() || xLimit.value() > aBuffer.mPlanes[0].mStride ||
      !yLimit.isValid() || yLimit.value() > aBuffer.mPlanes[0].mHeight)
  {
    // The specified picture dimensions can't be contained inside the video
    // frame, we'll stomp memory if we try to copy it. Fail.
    NS_WARNING("Overflowing picture rect");
    return nullptr;
  }

  nsAutoPtr<VideoData> v(new VideoData(aOffset,
                                       aTime,
                                       aDuration,
                                       aKeyframe,
                                       aTimecode,
                                       aInfo.mDisplay));
  const YCbCrBuffer::Plane &Y = aBuffer.mPlanes[0];
  const YCbCrBuffer::Plane &Cb = aBuffer.mPlanes[1];
  const YCbCrBuffer::Plane &Cr = aBuffer.mPlanes[2];

  if (!aImage) {
    // Currently our decoder only knows how to output to PLANAR_YCBCR
    // format.
    ImageFormat format[2] = {PLANAR_YCBCR, GRALLOC_PLANAR_YCBCR};
    if (IsYV12Format(Y, Cb, Cr)) {
      v->mImage = aContainer->CreateImage(format, 2);
    } else {
      v->mImage = aContainer->CreateImage(format, 1);
    }
  } else {
    v->mImage = aImage;
  }

  if (!v->mImage) {
    return nullptr;
  }
  NS_ASSERTION(v->mImage->GetFormat() == PLANAR_YCBCR ||
               v->mImage->GetFormat() == GRALLOC_PLANAR_YCBCR,
               "Wrong format?");
  PlanarYCbCrImage* videoImage = static_cast<PlanarYCbCrImage*>(v->mImage.get());

  PlanarYCbCrData data;
  data.mYChannel = Y.mData + Y.mOffset;
  data.mYSize = gfxIntSize(Y.mWidth, Y.mHeight);
  data.mYStride = Y.mStride;
  data.mYSkip = Y.mSkip;
  data.mCbChannel = Cb.mData + Cb.mOffset;
  data.mCrChannel = Cr.mData + Cr.mOffset;
  data.mCbCrSize = gfxIntSize(Cb.mWidth, Cb.mHeight);
  data.mCbCrStride = Cb.mStride;
  data.mCbSkip = Cb.mSkip;
  data.mCrSkip = Cr.mSkip;
  data.mPicX = aPicture.x;
  data.mPicY = aPicture.y;
  data.mPicSize = gfxIntSize(aPicture.width, aPicture.height);
  data.mStereoMode = aInfo.mStereoMode;

  videoImage->SetDelayedConversion(true);
  if (!aImage) {
    videoImage->SetData(data);
  } else {
    videoImage->SetDataNoCopy(data);
  }

  return v.forget();
}

VideoData* VideoData::Create(VideoInfo& aInfo,
                             ImageContainer* aContainer,
                             int64_t aOffset,
                             int64_t aTime,
                             int64_t aDuration,
                             const YCbCrBuffer& aBuffer,
                             bool aKeyframe,
                             int64_t aTimecode,
                             nsIntRect aPicture)
{
  return Create(aInfo, aContainer, nullptr, aOffset, aTime, aDuration, aBuffer,
                aKeyframe, aTimecode, aPicture);
}

VideoData* VideoData::Create(VideoInfo& aInfo,
                             Image* aImage,
                             int64_t aOffset,
                             int64_t aTime,
                             int64_t aDuration,
                             const YCbCrBuffer& aBuffer,
                             bool aKeyframe,
                             int64_t aTimecode,
                             nsIntRect aPicture)
{
  return Create(aInfo, nullptr, aImage, aOffset, aTime, aDuration, aBuffer,
                aKeyframe, aTimecode, aPicture);
}

VideoData* VideoData::CreateFromImage(VideoInfo& aInfo,
                                      ImageContainer* aContainer,
                                      int64_t aOffset,
                                      int64_t aTime,
                                      int64_t aDuration,
                                      const nsRefPtr<Image>& aImage,
                                      bool aKeyframe,
                                      int64_t aTimecode,
                                      nsIntRect aPicture)
{
  nsAutoPtr<VideoData> v(new VideoData(aOffset,
                                       aTime,
                                       aDuration,
                                       aKeyframe,
                                       aTimecode,
                                       aInfo.mDisplay));
  v->mImage = aImage;
  return v.forget();
}

#ifdef MOZ_OMX_DECODER
VideoData* VideoData::Create(VideoInfo& aInfo,
                             ImageContainer* aContainer,
                             int64_t aOffset,
                             int64_t aTime,
                             int64_t aDuration,
                             mozilla::layers::GraphicBufferLocked* aBuffer,
                             bool aKeyframe,
                             int64_t aTimecode,
                             nsIntRect aPicture)
{
  if (!aContainer) {
    // Create a dummy VideoData with no image. This gives us something to
    // send to media streams if necessary.
    nsAutoPtr<VideoData> v(new VideoData(aOffset,
                                         aTime,
                                         aDuration,
                                         aKeyframe,
                                         aTimecode,
                                         aInfo.mDisplay));
    return v.forget();
  }

  // The following situations could be triggered by invalid input
  if (aPicture.width <= 0 || aPicture.height <= 0) {
    NS_WARNING("Empty picture rect");
    return nullptr;
  }

  // Ensure the picture size specified in the headers can be extracted out of
  // the frame we've been supplied without indexing out of bounds.
  CheckedUint32 xLimit = aPicture.x + CheckedUint32(aPicture.width);
  CheckedUint32 yLimit = aPicture.y + CheckedUint32(aPicture.height);
  if (!xLimit.isValid() || !yLimit.isValid())
  {
    // The specified picture dimensions can't be contained inside the video
    // frame, we'll stomp memory if we try to copy it. Fail.
    NS_WARNING("Overflowing picture rect");
    return nullptr;
  }

  nsAutoPtr<VideoData> v(new VideoData(aOffset,
                                       aTime,
                                       aDuration,
                                       aKeyframe,
                                       aTimecode,
                                       aInfo.mDisplay));

  ImageFormat format = GRALLOC_PLANAR_YCBCR;
  v->mImage = aContainer->CreateImage(&format, 1);
  if (!v->mImage) {
    return nullptr;
  }
  NS_ASSERTION(v->mImage->GetFormat() == GRALLOC_PLANAR_YCBCR,
               "Wrong format?");
  typedef mozilla::layers::GrallocImage GrallocImage;
  GrallocImage* videoImage = static_cast<GrallocImage*>(v->mImage.get());
  GrallocImage::GrallocData data;

  data.mPicSize = gfxIntSize(aPicture.width, aPicture.height);
  data.mGraphicBuffer = aBuffer;

  videoImage->SetData(data);

  return v.forget();
}
#endif  // MOZ_OMX_DECODER

} // namespace mozilla