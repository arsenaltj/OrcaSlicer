#include "libslic3r/Model.hpp"
#include <iostream>
int main(int argc, char **argv)
{
    if (argc != 2) { std::cerr << "usage: orca_mobile_stl_gate model.stl\n"; return 2; }
    try {
        Slic3r::Model model = Slic3r::Model::read_from_file(argv[1]);
        if (model.objects.empty()) { std::cerr << "empty model\n"; return 3; }
        std::cout << "loaded objects=" << model.objects.size() << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "gate failed: " << e.what() << "\n";
        return 1;
    }
}
