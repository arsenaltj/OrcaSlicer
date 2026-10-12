#include "ModelLibraryLoaderFactory.hpp"
#include "ModelLibraryRepository.hpp"
#include "ModelLibraryThumbnail.hpp"

namespace Slic3r::GUI {
std::unique_ptr<ModelLibraryLoader> make_model_library_loader()
{
    auto cache = std::make_shared<ModelLibraryThumbnailCache>();
    return std::make_unique<ModelLibraryLoader>(ModelLibraryLoader::Ports {
        ModelLibraryRepository::read,
        [cache](const ModelLibraryEntry& entry, int edge, const std::atomic<bool>& cancelled) {
            ModelLibraryLoader::Thumbnail thumbnail {0, 0};
            const auto image = cache->load(entry.ai_image_path, entry.reference_image_path, edge, cancelled);
            if (image.IsOk() && !cancelled) {
                thumbnail.width = image.GetWidth(); thumbnail.height = image.GetHeight();
                const size_t pixels = size_t(thumbnail.width) * thumbnail.height;
                thumbnail.rgb.assign(image.GetData(), image.GetData() + pixels * 3);
                if (image.HasAlpha()) thumbnail.alpha.assign(image.GetAlpha(), image.GetAlpha() + pixels);
            }
            return thumbnail;
        }
    });
}
}
