import '../../godot_dart.dart';

class GodotDartNativeBindings {
  @pragma('vm:external-name', 'GodotDartNativeBindings::bindClass')
  external void bindClass(
    Type type,
    TypeInfo typeInfo,
  );

  @pragma('vm:external-name', 'GodotDartNativeBindings::bindMethod')
  external void bindMethod(TypeInfo typeInfo, String methodName,
      TypeInfo returnType, List<TypeInfo> argTypes);
}

// Potentially move this, just here for convenience
@pragma('vm:entry-point')
Variant _convertToVariant(Object? object) {
  return convertToVariant(object);
}
