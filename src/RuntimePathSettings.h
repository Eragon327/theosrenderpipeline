#pragma once

#include "NvidiaBaselinePolicy.h"
#include "FrameGen/NeuralRuntimeFiles.h"
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
            neural = SourceDLSSG::NormalizeNeuralRuntimeSetting(ini.GetValue("Experimental", "NeuralRenderingRuntimePath", ""),
                ini.GetBoolValue("Experimental", "NeuralRenderingRuntimeExplicit", false));
        }

        [[nodiscard]] RuntimePathSettings Resolve(const std::filesystem::path& pluginDirectory) const
        {
            return {ResolveRuntimePath(streamline, pluginDirectory).string(),
                SourceDLSSG::ResolveNeuralRuntimeSetting(neural, pluginDirectory).string()};
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
