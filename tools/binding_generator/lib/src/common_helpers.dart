import 'dart:io';

import 'string_extensions.dart';
import 'type_helpers.dart';

class GodotApiInfo {
  Map<String, dynamic> raw = <String, dynamic>{};

  Map<String, Map<String, dynamic>> builtinClasses = {};
  Map<String, Map<String, dynamic>> engineClasses = {};
  Set<String> singletons = {};
  Map<String, Map<String, dynamic>> nativeStructures = {};

  GodotApiInfo.fromJson(Map<String, dynamic> api) {
    raw = api;

    for (Map<String, dynamic> builtin in api['builtin_classes']) {
      final String name = builtin['name'];
      builtinClasses[name] = builtin;
    }

    for (Map<String, dynamic> engine in api['classes']) {
      final String name = engine['name'];
      engineClasses[name] = engine;
    }

    for (Map<String, dynamic> singleton in api['singletons']) {
      final String name = singleton['name'];
      singletons.add(name);
    }

    for (Map<String, dynamic> nativeStructure in api['native_structures']) {
      // TODO: These probably need special processing
      final String name = nativeStructure['name'];
      nativeStructures[name] = nativeStructure;
    }
  }
}

const String header = '''// AUTO GENERATED FILE, DO NOT EDIT.
//
// Generated by `godot_dart binding_generator`.
// ignore_for_file: duplicate_import
// ignore_for_file: unused_import
// ignore_for_file: unnecessary_import
// ignore_for_file: unused_field
// ignore_for_file: non_constant_identifier_names
''';

void writeImports(IOSink out, GodotApiInfo api, Map<String, dynamic> classApi,
    bool forVariant) {
  final String className = classApi['name'];

  out.write('''
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import '../../core/core_types.dart';
import '../../core/gdextension_ffi_bindings.dart';
import '../../core/gdextension.dart';
import '../../core/type_info.dart';
import '${forVariant ? '' : '../variant/'}string_name.dart';
${forVariant ? '' : "import '../../variant/variant.dart';"}

''');

  final usedClasses = getUsedTypes(classApi);
  for (var used in usedClasses) {
    if (used != className) {
      if (used == 'Object') {
        out.write("import '../../core/object.dart';\n");
      } else if (used == 'Variant') {
        out.write("import '../../variant/variant.dart';\n");
      } else if (used == 'TypedArray') {
        out.write("import '../../variant/typed_array.dart';\n");
      } else if (used == 'GlobalConstants') {
        out.write("import '../global_constants.dart';\n");
      } else {
        var prefix = '';
        if (!forVariant && api.builtinClasses.containsKey(used)) {
          prefix = '../variant/';
        }
        out.write("import '$prefix${used.toSnakeCase()}.dart';\n");
      }
    }
  }
}

void argumentAllocation(Map<String, dynamic> argument, IOSink out) {
  if (!argumentNeedsAllocation(argument)) return;

  var type = argument['type'] as String;
  var name = escapeName(argument['name'] as String);
  var ffiType = getFFIType(type);
  out.write(
      '      final ${name.toLowerCamelCase()}Ptr = arena.allocate<$ffiType>(sizeOf<$ffiType>())..value = ${name.toLowerCamelCase()};\n');
}

bool writeReturnAllocation(String returnType, IOSink out) {
  final nativeType = getFFIType(returnType);
  String indent = '      ';
  out.write(indent);
  if (nativeType == null) {
    out.write('final retPtr = retVal.opaque.cast();\n');
    return false;
  } else {
    out.write(
        'final retPtr = arena.allocate<$nativeType>(sizeOf<$nativeType>());\n');
    return true;
  }
}

void withAllocationBlock(
  List<dynamic> arguments,
  String? dartReturnType,
  IOSink out,
  void Function(String indent) writeBlock,
) {
  var indent = '';
  var needsArena = dartReturnType != null ||
      arguments.any((dynamic arg) => argumentNeedsAllocation(arg));
  if (needsArena) {
    indent = '  ';
    out.write('''
    using((arena) {
''');
    for (Map<String, dynamic> arg in arguments) {
      argumentAllocation(arg, out);
    }
  }
  writeBlock(indent);
  if (needsArena) {
    out.write('    });\n');
  }
}

void argumentFree(Map<String, dynamic> argument, IOSink out) {
  if (!argumentNeedsAllocation(argument)) return;

  var name = escapeName(argument['name'] as String);
  out.write('    malloc.free(${name.toLowerCamelCase()}Ptr);\n');
}

String getArgumentDeclaration(Map<String, dynamic> argument) {
  final correctedType = getCorrectedType(argument['type'] as String);
  final name = escapeName(argument['name'] as String);
  return '$correctedType ${name.toLowerCamelCase()}';
}

/// Generate a constructor name from arguments types. In the case
/// of a single argument constructor of the same type, the constructor
/// is called 'copy'. Otherwise it is named '.from{ArgType1}{ArgType2}'
String getConstructorName(String type, Map<String, dynamic> constructor) {
  var arguments = constructor['arguments'] as List?;
  if (arguments != null) {
    if (arguments.length == 1) {
      var argument = arguments[0] as Map<String, dynamic>;
      final argType = argument['type'] as String;
      if (argType == type) {
        return '.copy';
      } else if (argType == 'String') {
        return '.fromGDString';
      }
      return '.from${argument['type']}';
    } else {
      var name = '.from';
      for (final arg in arguments) {
        var argName = escapeName((arg['name'] as String)).toLowerCamelCase();
        name += argName[0].toUpperCase() + argName.substring(1);
      }
      return name;
    }
  }

  return '';
}

String makeSignature(Map<String, dynamic> functionData) {
  var modifiers = '';
  var returnType = getDartReturnType(functionData) ?? 'void';

  if (functionData['is_static'] == true) {
    modifiers = 'static ';
  }

  var methodName =
      escapeMethodName((functionData['name'] as String).toLowerCamelCase());

  var signature = '$modifiers$returnType $methodName(';

  final List<dynamic>? parameters = functionData['arguments'];
  if (parameters != null) {
    List<String> paramSignature = [];

    for (int i = 0; i < parameters.length; ++i) {
      Map<String, dynamic> parameter = parameters[i];
      final type = getCorrectedType(parameter['type'], meta: parameter['meta']);

      // TODO: Default values
      var paramName =
          escapeName((parameter['name'] as String)).toLowerCamelCase();
      paramSignature.add('$type $paramName');
    }
    signature += paramSignature.join(', ');
  }

  signature += ')';

  return signature;
}

List<String> getUsedTypes(Map<String, dynamic> api) {
  var usedTypes = <String>{};
  if (api.containsKey('constructors')) {
    for (Map<String, dynamic> constructor in api['constructors']) {
      if (constructor.containsKey('arguments')) {
        for (Map<String, dynamic> arg in constructor['arguments']) {
          usedTypes.add(arg['type']);
        }
      }
    }
  }

  if (api.containsKey('methods')) {
    for (Map<String, dynamic> method in api['methods']) {
      if (method.containsKey('arguments')) {
        for (Map<String, dynamic> arg in method['arguments']) {
          usedTypes.add(arg['type']);
        }
      }
      if (method.containsKey('return_type')) {
        usedTypes.add(method['return_type']);
      } else if (method.containsKey('return_value')) {
        final returnValue = method['return_value'] as Map<String, dynamic>;
        usedTypes.add(returnValue['type']);
      }
    }
  }

  if (api.containsKey('members')) {
    if (api.containsKey('members')) {
      for (Map<String, dynamic> member in api['members']) {
        usedTypes.add(member['type']);
      }
    }
  }

  // Typed arrays and enums
  if (usedTypes.any((e) => e.startsWith('typedarray::'))) {
    final typedArraySet = <String>{};
    for (var type in usedTypes) {
      if (type.startsWith('typedarray::')) {
        final typeParameter = type.split('::')[1];
        typedArraySet.add(typeParameter);
      }
    }
    usedTypes.removeWhere((e) => e.startsWith('typedarray::'));
    usedTypes.addAll(typedArraySet);
    usedTypes.add('TypedArray');
  }

  final enumAndBitfieldTypes = <String>[];
  for (var type in usedTypes
      .where((e) => e.startsWith('enum::') || e.startsWith('bitfield::'))) {
    if (type.contains('.')) {
      final parentClass = type
          .replaceAll('enum::', '')
          .replaceAll('bitfield::', '')
          .split('.')
          .first;
      // Special case -- enum::Variant.Type is held in GlobalConstants
      if (parentClass == 'Variant') {
        enumAndBitfieldTypes.add('GlobalConstants');
      } else {
        enumAndBitfieldTypes.add(parentClass);
      }
    } else {
      enumAndBitfieldTypes.add('GlobalConstants');
    }
  }
  usedTypes
      .removeWhere((e) => e.startsWith('enum::') || e.startsWith('bitfield::'));
  usedTypes.addAll(enumAndBitfieldTypes);

  // Remove pointer types for now
  usedTypes.removeWhere((e) => e.endsWith('*'));

  usedTypes.removeAll(dartTypes);
  // Already included
  usedTypes.remove('StringName');
  usedTypes.remove('GodotObject');

  return usedTypes.toList();
}

String getTypeFromArgument(Map<String, dynamic> argument) {
  String type = argument['type'];
  String? meta = argument['meta'];

  type = type.replaceFirst('const ', '');
  if (type.endsWith('*')) {
    type = type.substring(0, type.length - 1);
  }

  // Handle typed arrays?
  type = getCorrectedType(type, meta: meta);

  return type;
}

String? getDartReturnType(Map<String, dynamic> method) {
  String? returnType;
  String? returnMeta;

  if (method.containsKey('return_type')) {
    returnType = getCorrectedType(method['return_type']);
  } else if (method.containsKey('return_value')) {
    final returnValue = method['return_value'] as Map<String, dynamic>;
    returnType = returnValue['type'];
    if (returnValue.containsKey('meta')) {
      returnMeta = method['return_value']['meta'];
    }

    returnType = getCorrectedType(returnType!, meta: returnMeta);
    if (returnType == 'GDString') {
      returnType = 'String';
    }
  }

  return returnType;
}

void writeEnum(Map<String, dynamic> godotEnum, String? inClass, IOSink out) {
  var enumName = getEnumName(godotEnum['name'], inClass);
  out.write('enum $enumName {\n');
  List<String> values = [];
  for (Map<String, dynamic> value in godotEnum['values']) {
    final name = (value['name'] as String).toLowerCamelCase();
    values.add('  $name(${value['value']})');
  }
  out.write(values.join(',\n'));
  out.write(';\n');

  out.write('''

  final int value;
  const $enumName(this.value);
}

''');
}