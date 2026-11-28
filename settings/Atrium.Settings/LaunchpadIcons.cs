using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace Atrium.Settings;

// The Launchpad entry's own icon (native/core/settings.h: launchpadIcon): the picture file the user
// chose, read as the core reads it (the whole picture, turned upright, scaled to fit) for the page's
// row and the bar previews. Null: the built-in mark (no file, or a file that cannot be read).
internal static class LaunchpadIcons
{
    // The bar's icon square at its largest (64 DIP at 200 %): enough for every preview here.
    private const int MaxSide = 128;
    // Few pictures in use at once; a new pick may drop the old ones and their pixels.
    private const int MaxCached = 8;
    private static readonly Dictionary<string, WriteableBitmap?> Cache = new(StringComparer.OrdinalIgnoreCase);

    // The picture at most MaxSide across, or null when it cannot be read.
    internal static WriteableBitmap? Preview(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        if (Cache.TryGetValue(path, out var cached)) return cached;
        var bitmap = Decode(path);
        if (Cache.Count >= MaxCached) Cache.Clear();
        Cache[path] = bitmap;
        return bitmap;
    }

    private static WriteableBitmap? Decode(string path)
    {
        if (!File.Exists(path)) return null;
        try
        {
            var file = StorageFile.GetFileFromPathAsync(path).AsTask().GetAwaiter().GetResult();
            using var stream = file.OpenReadAsync().AsTask().GetAwaiter().GetResult();
            var decoder = BitmapDecoder.CreateAsync(stream).AsTask().GetAwaiter().GetResult();
            uint w = decoder.PixelWidth, h = decoder.PixelHeight;
            if (w == 0 || h == 0) return null;
            double k = Math.Min(1.0, (double)MaxSide / Math.Max(w, h));
            bool turned = decoder.OrientedPixelWidth != decoder.PixelWidth;
            // The transform scales the stored picture; the decoder turns it upright afterwards (EXIF).
            var transform = new BitmapTransform
            {
                ScaledWidth = (uint)Math.Max(1, Math.Round(w * k)),
                ScaledHeight = (uint)Math.Max(1, Math.Round(h * k)),
                InterpolationMode = BitmapInterpolationMode.Fant,
            };
            var data = decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, transform,
                ExifOrientationMode.RespectExifOrientation, ColorManagementMode.DoNotColorManage).AsTask().GetAwaiter().GetResult();
            int width = (int)(turned ? transform.ScaledHeight : transform.ScaledWidth);
            int height = (int)(turned ? transform.ScaledWidth : transform.ScaledHeight);
            byte[] pixels = data.DetachPixelData();
            var bitmap = new WriteableBitmap(width, height);
            using (var target = bitmap.PixelBuffer.AsStream()) target.Write(pixels, 0, pixels.Length);
            bitmap.Invalidate();
            return bitmap;
        }
        catch (Exception e)
        {
            System.Diagnostics.Debug.WriteLine("Launchpad icon unreadable: " + e.Message);
            return null;
        }
    }
}
