#include "stream_manager.h"
#include <iostream>

bool StreamManager::addStream(const std::string& id, const std::string& url) {
    std::cout << "Adding stream: " << id << " -> " << url << "\n";
    // TODO: create decoder instance
    return true;
}

bool StreamManager::removeStream(const std::string& id) {
    std::cout << "Removing stream: " << id << "\n";
    return true;
}

std::vector<StreamInfo> StreamManager::list() const {
    return {};
}
