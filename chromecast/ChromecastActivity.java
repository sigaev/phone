package dev.demo.chromecast;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;
import android.util.AtomicFile;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

// NativeActivity has no native activity-result callback. This small bridge owns
// the system photo picker and image decoding; the UI and renderer stay in C++.
public final class ChromecastActivity extends NativeActivity {
  private static final int PICK_WALLPAPER = 1;
  private static final int MAX_IMAGE_SIZE = 2048;
  private final ExecutorService images = Executors.newSingleThreadExecutor();
  private long nativeState;
  private int imageRequest;

  private static native void wallpaperReady(long state, Bitmap bitmap);

  private File wallpaperFile() { return new File(getFilesDir(), "wallpaper.png"); }

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    if (wallpaperFile().exists()) loadWallpaper(null);
  }

  // Called from the native Wallpaper button. No storage permission is needed.
  public void chooseWallpaper() {
    new AlertDialog.Builder(this)
        .setTitle("Wallpaper")
        .setItems(new String[] {"Choose image for ripples", "System wallpaper (still)"},
            (dialog, which) -> {
              if (which == 0) {
                Intent pick = new Intent(MediaStore.ACTION_PICK_IMAGES);
                pick.setType("image/*");
                startActivityForResult(pick, PICK_WALLPAPER);
              } else {
                ++imageRequest;
                images.execute(() -> new AtomicFile(wallpaperFile()).delete());
                if (nativeState != 0) wallpaperReady(nativeState, null);
              }
            })
        .show();
  }

  @Override
  protected void onActivityResult(int request, int result, Intent data) {
    super.onActivityResult(request, result, data);
    if (request == PICK_WALLPAPER && result == RESULT_OK && data != null && data.getData() != null)
      loadWallpaper(data.getData());
  }

  private void loadWallpaper(Uri uri) {
    final int request = ++imageRequest;
    images.execute(() -> {
      Bitmap image = null;
      try {
        ImageDecoder.Source source = uri == null
            ? ImageDecoder.createSource(wallpaperFile())
            : ImageDecoder.createSource(getContentResolver(), uri);
        image = ImageDecoder.decodeBitmap(source, (decoder, info, ignored) -> {
          int width = info.getSize().getWidth(), height = info.getSize().getHeight();
          float scale = Math.min(1f, (float) MAX_IMAGE_SIZE / Math.max(width, height));
          decoder.setTargetSize(
              Math.max(1, Math.round(width * scale)), Math.max(1, Math.round(height * scale)));
          decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
          decoder.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.SRGB));
        });
        // High-bit-depth images may still decode to F16. Native uploads use RGBA8.
        if (image.getConfig() != Bitmap.Config.ARGB_8888) {
          Bitmap converted = image.copy(Bitmap.Config.ARGB_8888, false);
          image.recycle();
          image = converted;
          if (image == null) {
            showImageError(request);
            return;
          }
        }
        if (uri != null) {
          AtomicFile saved = new AtomicFile(wallpaperFile());
          FileOutputStream stream = null;
          try {
            stream = saved.startWrite();
            if (image.compress(Bitmap.CompressFormat.PNG, 100, stream)) {
              saved.finishWrite(stream);
            } else {
              saved.failWrite(stream);
              image.recycle();
              showImageError(request);
              return;
            }
          } catch (IOException error) {
            saved.failWrite(stream);
            image.recycle();
            showImageError(request);
            return;
          }
        }
        final Bitmap ready = image;
        runOnUiThread(() -> {
          if (nativeState != 0 && request == imageRequest) wallpaperReady(nativeState, ready);
          ready.recycle();
        });
      } catch (IOException | RuntimeException | OutOfMemoryError error) {
        if (image != null) image.recycle();
        showImageError(request);
      }
    });
  }

  private void showImageError(int request) {
    runOnUiThread(() -> {
      if (nativeState != 0 && request == imageRequest)
        Toast.makeText(this, "Could not load that image. Try another.", Toast.LENGTH_LONG).show();
    });
  }

  @Override
  protected void onDestroy() {
    nativeState = 0;
    images.shutdown();
    super.onDestroy();
  }
}
