#ifndef FID_HPP
#define FID_HPP

#include <string>

struct FID {
    int Volume_number;       // I only want the root, different user folders within usr and the projects folder to have a seperate volume number
                                // which are (root_dir), (root_dir/usr/Eric   root_dir/usr/Brian ...), root_dir/projects
    int Vnode_number;
    int uniquifier;

    // operator== ---> operator overloading (required for unordered_map)
    bool operator==(const FID& other) const {
        return Volume_number == other.Volume_number &&
               Vnode_number == other.Vnode_number &&
               uniquifier == other.uniquifier;
    }
}; 

// template specialisation: Specializing the std::hash template for FID (required for unordered_map)
namespace std {
    template <>
    struct hash<FID> {
        size_t operator()(const FID& fid) const {
            // Combine hashes of all three fields
            size_t h1 = hash<int>()(fid.Volume_number);
            size_t h2 = hash<int>()(fid.Vnode_number);
            size_t h3 = hash<int>()(fid.uniquifier);
            
            // Combine using XOR and bit shifting
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
}

#endif