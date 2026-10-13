#pragma once

#include "NvidiaBaselinePolicy.h"
#include <filesystem>
#include <string>

namespace TheosRenderPipeline
{
    struct RuntimePathSettings
    {
        std::string streamline;
        std::string neural;

        template<class Ini> void Load(const Ini& ini)
        {
            streamline = ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", "");
            neural = ini.GetValue("Experimental", "NeuralRenderingRuntimePath", "");
        }

        [[nodiscard]] RuntimePathSettings Resolve(const std::filesystem::path& pluginDirectory) const
        {
            return {ResolveRuntimePath(streamline, pluginDirectory).string(),
                ResolveRuntimePath(neural, pluginDirectory).string()};
        }

        template<class Ini> void Store(Ini& ini) const
        {
            // Seed missing values only. The NR runtime is a menu choice that
            // StorePreferences writes afterwards; Streamline stays startup-owned.
            if (!ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", nullptr)) {
                ini.SetValue("Experimental", "SourceDLSSGStreamlineDirectory", streamline.c_str());
            }
            if (!ini.GetValue("Experimental", "NeuralRenderingRuntimePath", nullptr)) {
                ini.SetValue("Experimental", "NeuralRenderingRuntimePath", neural.c_str());
            }
        }
    };
}
