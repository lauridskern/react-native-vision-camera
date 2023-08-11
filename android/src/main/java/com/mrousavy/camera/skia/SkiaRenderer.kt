package com.mrousavy.camera.skia

import android.graphics.SurfaceTexture
import android.hardware.HardwareBuffer
import android.media.Image
import android.os.Build
import android.os.Looper
import android.util.Log
import android.view.Surface
import com.facebook.jni.HybridData
import com.facebook.proguard.annotations.DoNotStrip
import com.mrousavy.camera.CameraError
import com.mrousavy.camera.CameraQueues
import com.mrousavy.camera.UnknownCameraError
import java.io.Closeable
import java.lang.RuntimeException
import java.util.concurrent.locks.ReentrantLock

@Suppress("KotlinJniMissingFunction")
class SkiaRenderer: Closeable {
  companion object {
    private const val TAG = "SkiaRenderer"
  }
  @DoNotStrip
  private var mHybridData: HybridData
  private var hasNewFrame = false

  val thread = CameraQueues.previewQueue.handler

  init {
    mHybridData = initHybrid()
  }

  override fun close() {
    hasNewFrame = false
    thread.post {
      synchronized(this) {
        destroyOutputSurface()
        mHybridData.resetNative()
      }
    }
  }

  fun setPreviewSurface(surface: Surface) {
    synchronized(this) {
      setOutputSurface(surface)
    }
  }

  fun setPreviewSurfaceSize(width: Int, height: Int) {
    synchronized(this) {
      setOutputSurfaceSize(width, height)
    }
  }

  fun destroyPreviewSurface() {
    synchronized(this) {
      destroyOutputSurface()
    }
  }

  fun setInputSurfaceSize(width: Int, height: Int) {
    synchronized(this) {
      setInputTextureSize(width, height)
    }
  }

  /**
   * Called on every Camera Frame (1..240 FPS)
   */
  fun onCameraFrame(image: Image) {
    synchronized(this) {
      Log.i(TAG, "Getting HWB")
      val hardwareBuffer = image.hardwareBuffer ?: throw Error("Image doesn't have a backing Hardware Buffer!")
      Log.i(TAG, "HWB ! ${hardwareBuffer.width}")
      renderCameraFrameToOffscreenCanvas(hardwareBuffer)
      hasNewFrame = true
    }
  }

  /**
   * Called on every UI Frame (60 FPS)
   */
  fun onPreviewFrame() {
    synchronized(this) {
      renderLatestFrameToPreview()
      hasNewFrame = false
    }
  }

  private external fun initHybrid(): HybridData

  private external fun renderCameraFrameToOffscreenCanvas(hardwareBuffer: Any)
  private external fun renderLatestFrameToPreview()
  private external fun setInputTextureSize(width: Int, height: Int)
  private external fun setOutputSurface(surface: Any)
  private external fun setOutputSurfaceSize(width: Int, height: Int)
  private external fun destroyOutputSurface()
}
