#!/usr/bin/env python3.10
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0 WITH Qt-GPL-exception-1.0

import argparse
import os
# for installing use pip3 install robotpy-cppheaderparse
import CppHeaderParser

def parse_arguments():
    parser = argparse.ArgumentParser(description='Clazy checks header file \
    generator')
    parser.add_argument('--clang-format-header-file', help='path to \
    Format.h usually /usr/lib/llvm-x/include/clang/Format/Format.h',
                        default=None, dest='options_header', required=True)
    return parser.parse_args()


def full_header_content(header_code):
    return '''// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

// THIS FILE IS AUTOMATICALLY GENERATED. DO NOT EDIT!

#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;
QT_END_NAMESPACE

namespace ClangFormat {

class ClangFormatChecks : public QWidget
{
    Q_OBJECT
public:
    ClangFormatChecks(QWidget *parent = nullptr);

private:
''' + header_code + '''
};

} //ClangFormat
'''


def full_source_content(source_code, layout_code):
    return '''// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

// THIS FILE IS AUTOMATICALLY GENERATED. DO NOT EDIT!

#include "clangformatchecks.h"

#include <utils/layoutbuilder.h>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

using namespace Utils;

using namespace ClangFormat;

ClangFormatChecks::ClangFormatChecks(QWidget *parent)
    : QWidget(parent)
{
''' + source_code + '''
    using namespace Layouting;

    Form {
''' + layout_code + '''    }.attachTo(this);
}
'''

#   Combobox UI
def combobox_header(name):
    header = "    QComboBox *m_" + name + " = nullptr;\n"
    return header

def combobox_source(name, values, offset):
    source = ""
    source += "    m_" + name + " = new QComboBox(this);\n"

    list = ""
    for value in values:
        list += "\"" + value + "\","

    source += "    m_" + name + "->addItems({" + list + "});\n"
    source += "    m_" + name + "->setObjectName(\"" + name + "\");\n\n"
    return source

def combobox_source_bool(name, offset):
    return combobox_source(name, ["Default", "true", "false"], offset)

def combobox_layout(name, offset):
    layout = "        new QLabel(\"" + offset + name + "\"), m_" + name + ", br,\n"
    return layout

#   String UI
def string_header(name):
    header = "    QLineEdit *m_" + name + " = nullptr;\n"
    header += "    QPushButton *m_set" + name + " = nullptr;\n"
    return header

def string_source(name, offset):
    source = ""
    source += "    m_" + name + " = new QLineEdit(this);\n"
    source += "    m_" + name + "->setObjectName(\"" + name + "\");\n"
    source += "    m_set" + name + " = new QPushButton(\"Set\", this);\n\n"
    source += "    m_set" + name + "->setObjectName(\"set" + name + "\");\n"
#    source += "m_" + name + "->setObjectName(\"" + offset + name + "\");\n\n"
    return source

def string_layout(name, offset):
    layout = "        new QLabel(\"" + offset + name + "\"), Row {m_" + name + ", m_set" + name + "}, br,\n"
    return layout

#   Vector UI
def vector_header(name):
    header = "    QPlainTextEdit *m_" + name + " = nullptr;\n"
    header += "    QPushButton *m_set" + name + " = nullptr;\n"
    return header

def vector_source(name, offset):
    source = ""
    source += "    m_" + name + " = new QPlainTextEdit(this);\n"
    source += "    m_" + name + "->setObjectName(\"" + name + "\");\n"
    source += "    m_" + name + "->setFixedHeight(100);\n"
    source += "    m_set" + name + " = new QPushButton(\"Set\", this);\n\n"
    source += "    m_set" + name + "->setObjectName(\"set" + name + "\");\n"
#    source += "m_" + name + "->setObjectName(\"" + offset + name + "\");\n\n"
    return source

def vector_layout(name, offset):
    layout = "        new QLabel(\"" + offset + name + "\"), Row {m_" + name + ", m_set" + name + "}, br,\n"
    return layout

#   Struct Layout
def struct_layout(name, offset):
    layout = "        new QLabel(\"" + offset + name + "\"), br,\n"
    return layout


def in_list(list, type):
    for element in list:
        if element["name"] == type:
            return element;
    return

def create_private_variables(variables, enums, structs, offset = ""):
    header = ""
    source = ""
    layout = ""

    # create BasedOnStyle combobox ussually not presented in FormatStyle struct
    if offset == "":
        header += combobox_header("BasedOnStyle")
        source += combobox_source("BasedOnStyle", ["LLVM", "Google", "Chromium", "Mozilla", "WebKit", "Microsoft", "GNU"], offset)
        layout += combobox_layout("BasedOnStyle", offset)

    for variable in variables:
        if "doxygen" in variable.keys():
            if ("**deprecated**" in variable['doxygen']):
                continue;

        type = variable["type"]
        name = variable["name"]
        enum = in_list(enums, type)
        struct = in_list(structs, type)
        if enum:
            header += combobox_header(name)
            source += combobox_source(name, [value["name"].split("_")[1] for value in enum["values"]], offset)
            layout += combobox_layout(name, offset)
        elif struct:
            layout += struct_layout(name, offset)
            header_tmp, source_tmp, layout_tmp = create_private_variables(struct["properties"]["public"], enums, structs, "  ")
            header += header_tmp
            source += source_tmp
            layout += layout_tmp
        elif "std::string" == type or "unsigned" == type or "int" == type:
            header += string_header(name)
            source += string_source(name, offset)
            layout += string_layout(name, offset)
        elif "std::vector<std::string >" == type:
            header += vector_header(name)
            source += vector_source(name, offset)
            layout += vector_layout(name, offset)
        elif "bool" == type:
            header += combobox_header(name)
            source += combobox_source_bool(name, offset)
            layout += combobox_layout(name, offset);
    return header, source, layout


def main():
    arguments = parse_arguments()
    header = CppHeaderParser.CppHeader(arguments.options_header)

    enums = header.classes["FormatStyle"]["enums"]["public"]
    structs = header.classes["FormatStyle"]["nested_classes"]
    variables = header.classes["FormatStyle"]["properties"]["public"]

    current_path = os.path.dirname(os.path.abspath(__file__))
    source_path = os.path.abspath(os.path.join(current_path, '..', 'src',
                    'plugins', 'clangformat', 'clangformatchecks.cpp'))
    header_path = os.path.abspath(os.path.join(current_path, '..', 'src',
                    'plugins', 'clangformat', 'clangformatchecks.h'))

    header, source, layout = create_private_variables(variables, enums, structs)
    with open(source_path, 'w') as f:
        f.write(full_source_content(source, layout))

    with open(header_path, 'w') as f:
        f.write(full_header_content(header))


if __name__ == "__main__":
    main()
