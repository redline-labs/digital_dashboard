#ifndef PHONE_TYPE_H_
#define PHONE_TYPE_H_

#include <string_view>

enum class PhoneType {
    AndroidMirror = 1,
    CarPlay = 3,
    iPhoneMirror = 4,
    AndroidAuto = 5,
    HiCar = 6,
};


constexpr std::string_view phone_type_to_string(PhoneType type)
{
    switch (type)
    {
        case (PhoneType::AndroidMirror):
            return "AndroidMirror";

        case (PhoneType::CarPlay):
            return "CarPlay";

        case (PhoneType::iPhoneMirror):
            return "iPhoneMirror";

        case (PhoneType::AndroidAuto):
            return "AndroidAuto";

        case (PhoneType::HiCar):
        default:
            return "HiCar";
    }
}

#endif // PHONE_TYPE_H_