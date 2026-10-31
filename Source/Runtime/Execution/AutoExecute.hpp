#pragma once
#include <Runtime/Execution/ExecutionGuard.hpp>
#include <Core/Context.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>

namespace AutoExecute
{
    inline void ExecuteFolder()
    {
        try
        {
            if (SharedVariables::autoexecute_folder.empty() || !std::filesystem::exists(SharedVariables::autoexecute_folder))
                return;

            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(SharedVariables::autoexecute_folder))
            {
                if (entry.is_regular_file())
                {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".lua" || ext == ".luau" || ext == ".txt" || ext.empty())
                    {
                        files.push_back(entry.path());
                    }
                }
            }

            std::sort(files.begin(), files.end());

            for (const auto& filePath : files)
            {
                std::ifstream file(filePath, std::ios::binary);
                if (file.is_open())
                {
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    std::string script = buffer.str();

                    if (script.size() >= 3 && 
                        static_cast<unsigned char>(script[0]) == 0xEF && 
                        static_cast<unsigned char>(script[1]) == 0xBB && 
                        static_cast<unsigned char>(script[2]) == 0xBF)
                    {
                        script.erase(0, 3);
                    }

                    if (!script.empty())
                    {
                        TaskScheduler::RequestExecution(ExecutionGuard::Wrap(script));
                    }
                }
            }
        }
        catch (...)
        {
        }
    }
}
