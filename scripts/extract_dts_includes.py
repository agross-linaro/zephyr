#!/usr/bin/env python3

# vim: ai:ts=4:sw=4

import sys
from os import listdir, walk
import os
import re
import yaml
import pprint
import argparse

from devicetree import parse_file

from copy import deepcopy

# globals
compatibles = {}
phandles = {}
aliases = {}
chosen = {}
reduced = {}
defs = {}
structs = {}
struct_dict = {}
node_init_file = ""
sub_struct_count = 0

def convert_string_to_label(s):
  # Transmute ,- to _
  s = s.replace("-", "_");
  s = s.replace(",", "_");
  return s

def get_all_compatibles(d, name, comp_dict):
  if 'props' in d:
    compat = d['props'].get('compatible')
    enabled = d['props'].get('status')

  if enabled == "disabled":
    return comp_dict

  if compat != None:
    comp_dict[name] = compat

  if name != '/':
    name += '/'

  if isinstance(d,dict):
    if d['children']:
      for k,v in d['children'].items():
        get_all_compatibles(v, name + k, comp_dict)

  return comp_dict

def get_aliases(root):
  if 'children' in root:
    if 'aliases' in root['children']:
      for k,v in root['children']['aliases']['props'].items():
        aliases[v] = k

  return

def get_compat(node):

  compat = None

  if 'props' in node:
    compat = node['props'].get('compatible')

  if isinstance(compat, list):
    compat = compat[0]

  return compat

def get_chosen(root):

  if 'children' in root:
    if 'chosen' in root['children']:
      for k,v in root['children']['chosen']['props'].items():
        chosen[k] = v

  return

def get_phandles(root, name, handles):

  if 'props' in root:
    handle = root['props'].get('phandle')
    enabled = root['props'].get('status')

  if enabled == "disabled":
    return

  if handle != None:
    phandles[handle] = name

  if name != '/':
    name += '/'

  if isinstance(root, dict):
    if root['children']:
      for k,v in root['children'].items():
        get_phandles(v, name + k, handles)

  return

class Loader(yaml.Loader):
    def __init__(self, stream):
        self._root = os.path.realpath(stream.name)
        super(Loader, self).__init__(stream)
        Loader.add_constructor('!include', Loader.include)
        Loader.add_constructor('!import',  Loader.include)

    def include(self, node):
        if isinstance(node, yaml.ScalarNode):
            return self.extractFile(self.construct_scalar(node))

        elif isinstance(node, yaml.SequenceNode):
            result = []
            for filename in self.construct_sequence(node):
                result += self.extractFile(filename)
            return result

        elif isinstance(node, yaml.MappingNode):
            result = {}
            for k,v in self.construct_mapping(node).iteritems():
                result[k] = self.extractFile(v)
            return result

        else:
            print("Error:: unrecognised node type in !include statement")
            raise yaml.constructor.ConstructorError

    def extractFile(self, filename):
        filepath = os.path.join(os.path.dirname(self._root), filename)
        if not os.path.isfile(filepath):
            # we need to look in common directory
            # take path and back up 2 directories and tack on '/common/yaml'
            filepath = os.path.dirname(self._root).split('/')
            filepath = '/'.join(filepath[:-2])
            filepath = os.path.join(filepath + '/common/yaml', filename)
            with open(filepath, 'r') as f:
                return yaml.load(f, Loader)

def insert_defs(node_address, defs, new_defs, new_aliases):
  if node_address in defs:
    if 'aliases' in defs[node_address]:
      defs[node_address]['aliases'].update(new_aliases)
    else:
      defs[node_address]['aliases'] = new_aliases

    defs[node_address].update(new_defs)
  else:
    new_defs['aliases'] = new_aliases
    defs[node_address] = new_defs

  return

def insert_structs(node_address, deflabel, new_structs):

    if isinstance(new_structs, list):
        new_structs = new_structs[0]

    if node_address in structs:
        if deflabel in structs[node_address] and isinstance(structs[node_address][deflabel], dict):
            structs[node_address][deflabel].update(new_structs)
        else:
            structs[node_address][deflabel] = new_structs
    else:
        structs[node_address] = {}
        structs[node_address][deflabel] = new_structs

    return

def find_node_by_path(nodes, path):
  d = nodes
  for k in path[1:].split('/'):
    d = d['children'][k]

  return d

def compress_nodes(nodes, path):
  if 'props' in nodes:
    status = nodes['props'].get('status')

  if status == "disabled":
    return

  if isinstance(nodes, dict):
    reduced[path] = dict(nodes)
    reduced[path].pop('children', None)
    if path != '/':
      path += '/'
    if nodes['children']:
      for k,v in nodes['children'].items():
        compress_nodes(v, path + k)

  return

def find_parent_irq_node(node_address):
  address = ''

  for comp in node_address.split('/')[1:]:
    address += '/' + comp
    if 'interrupt-parent' in reduced[address]['props']:
      interrupt_parent = reduced[address]['props'].get('interrupt-parent')

  return reduced[phandles[interrupt_parent]]

def extract_interrupts(node_address, yaml, y_key, names, def_label):
  node = reduced[node_address]

  try:
    props = list(node['props'].get(y_key))
  except:
    props = [node['props'].get(y_key)]

  irq_parent = find_parent_irq_node(node_address)

  l_base = def_label.split('/')
  index = 0

  prop_structs = []
  while props:
    prop_def = {}
    prop_alias = {}
    l_idx = [str(index)]

    if y_key == 'interrupts-extended':
      cell_parent = reduced[phandles[props.pop(0)]]
      name = []
    else:
      try:
        name = [names.pop(0).upper()]
      except:
        name = []

      cell_parent = irq_parent

    cell_yaml = yaml[get_compat(cell_parent)]
    l_cell_prefix = [yaml[get_compat(irq_parent)].get('cell_string', []).upper()]

    cell_struct = {}
    cell_struct['members'] = cell_yaml['#cells']
    cell_struct['data'] = []
    cell_struct['defs'] = {'labels':[], 'aliases':[]}
    for i in range(cell_parent['props']['#interrupt-cells']):
      l_cell_name = [cell_yaml['#cells'][i].upper()]
      if l_cell_name == l_cell_prefix:
        l_cell_name = []

      l_fqn = '_'.join(l_base + l_cell_prefix + l_idx + l_cell_name)
      val = props.pop(0)
      prop_def[l_fqn] = val
      cell_struct['defs']['labels'].append(l_fqn)
      cell_struct['data'].append(val)
      if len(name):
        prop_alias['_'.join(l_base + name + l_cell_prefix)] = l_fqn
        cell_struct['defs']['aliases'].append('_'.join(l_base + name + l_cell_prefix))
    prop_structs.append(cell_struct)

    index += 1
    insert_defs(node_address, defs, prop_def, prop_alias)
  insert_structs(node_address, 'interrupts', prop_structs)

  return

def extract_reg_prop(node_address, names, defs, def_label, div, post_label):
  node = reduced[node_address]

  props = list(reduced[node_address]['props']['reg'])

  address_cells = reduced['/']['props'].get('#address-cells')
  size_cells = reduced['/']['props'].get('#size-cells')
  address = ''
  for comp in node_address.split('/')[1:]:
    address += '/' + comp
    address_cells = reduced[address]['props'].get('#address-cells', address_cells)
    size_cells = reduced[address]['props'].get('#size-cells', size_cells)

  if post_label is None:
    post_label = "BASE_ADDRESS"

  index = 0
  l_base = def_label.split('/')
  l_addr = ["BASE_ADDRESS"]
  l_size = ["SIZE"]

  prop_struct = []
  while props:
    prop_def = {}
    prop_alias = {}
    addr = 0
    size = 0
    l_idx = [str(index)]
    entry_struct = {}
    entry_struct['members'] = []
    entry_struct['data'] = []
    entry_struct['defs'] = {'labels':[], 'aliases':[]}

    try:
      name = [names.pop(0).upper()]
    except:
      name = []

    for x in range(address_cells):
      addr += props.pop(0) << (32 * x)
    for x in range(size_cells):
      size += props.pop(0) << (32 * x)

    l_addr_fqn = '_'.join(l_base + l_addr + l_idx)
    l_size_fqn = '_'.join(l_base + l_size + l_idx)
    prop_def[l_addr_fqn] = hex(addr)
    prop_def[l_size_fqn] = int(size / div)
    entry_struct['defs']['labels'].append(l_addr_fqn)
    entry_struct['defs']['labels'].append(l_size_fqn)
    if len(name):
      prop_alias['_'.join(l_base + name + l_addr)] = l_addr_fqn
      prop_alias['_'.join(l_base + name + l_size)] = l_size_fqn
      entry_struct['defs']['aliases'].append({'_'.join(l_base + name + l_addr): l_addr_fqn})
      entry_struct['defs']['aliases'].append({'_'.join(l_base + name + l_size): l_size_fqn})

    if index == 0:
      prop_alias['_'.join(l_base + l_addr)] = l_addr_fqn
      prop_alias['_'.join(l_base + l_size)] = l_size_fqn
      entry_struct['defs']['aliases'].append({'_'.join(l_base + l_addr): l_addr_fqn})
      entry_struct['defs']['aliases'].append({'_'.join(l_base + l_size): l_size_fqn})

    entry_struct['data'] = [hex(addr), int(size/div)]
    prop_struct.append(entry_struct)
    #prop_struct.append(hex(addr))
    #prop_struct.append(int(size/div))

    insert_defs(node_address, defs, prop_def, prop_alias)
    insert_structs(node_address, 'reg', prop_struct)

    # increment index for definition creation
    index += 1

  return

def extract_cells(node_address, yaml, y_key, names, index, prefix, def_label):
  try:
    props = list(reduced[node_address]['props'].get(y_key))
  except:
    props = [reduced[node_address]['props'].get(y_key)]

  cell_parent = reduced[phandles[props.pop(0)]]

  try:
    cell_yaml = yaml[get_compat(cell_parent)]
  except:
    raise Exception("Could not find yaml description for " + cell_parent['name'])

  try:
    name = names.pop(0).upper()
  except:
    name = []

  l_cell = [str(cell_yaml.get('cell_string',''))]
  l_base = def_label.split('/')
  l_base += prefix
  l_idx = [str(index)]

  prop_def = {}
  prop_alias = {}
  prop_struct = {}

  cell_struct = {}
  cell_struct['data'] = []
  cell_struct['members'] = cell_yaml['#cells']
  cell_struct['defs'] = {'labels':[], 'aliases':[]}
  for k in cell_parent['props'].keys():
    if k[0] == '#' and '-cells' in k:
      for i in range(cell_parent['props'].get(k)):
        l_cellname = [str(cell_yaml['#cells'][i]).upper()]
        if l_cell == l_cellname:
          label = l_base + l_cell + l_idx
        else:
          label = l_base + l_cell + l_cellname + l_idx
        label_name = l_base + name + l_cellname
        val = props.pop(0)
        cell_struct['data'].append(val)
        prop_def['_'.join(label)] = val
        cell_struct['defs']['labels'].append('_'.join(label))
        if len(name):
          prop_alias['_'.join(label_name)] = '_'.join(label)
          cell_struct['defs']['aliases'].append({'_'.join(label_name): '_'.join(label)})

        if index == 0:
          prop_alias['_'.join(label[:-1])] = '_'.join(label)
          cell_struct['defs']['aliases'].append({'_'.join(label[:-1]): '_'.join(label)})

    insert_defs(node_address, defs, prop_def, prop_alias)
    insert_structs(node_address, cell_yaml.get('cell_string'), cell_struct)

  # recurse if we have anything left
  if len(props):
    extract_cells(node_address, yaml, y_key, names, index + 1, prefix, def_label)

  return

def extract_pinctrl(node_address, yaml, pinconf, names, index, def_label):

  prop_list = []
  if not isinstance(pinconf,list):
    prop_list.append(pinconf)
  else:
    prop_list = list(pinconf)

  def_prefix = def_label.split('_')
  target_node = node_address

  prop_def = {}
  prop_struct = []
  for p in prop_list:
    pin_node_address = phandles[p]
    pin_entry = reduced[pin_node_address]
    parent_address = '/'.join(pin_node_address.split('/')[:-1])
    pin_subnode = '/'.join(pin_node_address.split('/')[-1:])
    pin_parent = reduced[parent_address]
    cell_yaml = yaml[get_compat(pin_parent)]
    cell_prefix = cell_yaml.get('cell_string', None)
    post_fix = []

    cell_struct = {}
    cell_struct['data'] = []
    cell_struct['struct name'] = cell_yaml['#struct'][0].get('name')
    cell_struct['members'] = pin_entry.get('label')
    cell_struct['defs'] = {'labels':[], 'aliases':[]}

    if cell_prefix != None:
      post_fix.append(cell_prefix)

    for subnode in reduced.keys():
      if pin_subnode in subnode and pin_node_address != subnode:
        # found a subnode underneath the pinmux handle
        node_label = subnode.split('/')[-2:]
        pin_label = def_prefix + post_fix + subnode.split('/')[-2:]

        for i, cells in enumerate(reduced[subnode]['props']):
            if len(cell_yaml['#cells']) == 2:
              key_label = list(pin_label) + [cell_yaml['#cells'][0]] + [str(i)]
              func_label = key_label[:-2] + [cell_yaml['#cells'][1]] + [str(i)]
              key_label = convert_string_to_label('_'.join(key_label)).upper()
              func_label = convert_string_to_label('_'.join(func_label)).upper()

              prop_def[key_label] = reduced[subnode]['props'][cells]
              prop_def[func_label] = reduced[subnode]['props'][cells]
              cell_struct['defs']['labels'].append(key_label)
              cell_struct['defs']['labels'].append(func_label)

            elif len(cell_yaml['#cells']) == 1:
              key_label = list(pin_label) + [cell_yaml['#cells'][0]] + [str(i)]
              key_label = convert_string_to_label('_'.join(key_label)).upper()

              prop_def[key_label] = reduced[subnode]['props'][cells]
              cell_struct['defs']['labels'].append(key_label)


        if len(cell_yaml['#cells']) == 2:
            pin_list=[]
            pin_list=reduced[subnode]['props'].get(cell_yaml['#cells'][0])
            for i in pin_list:
                cell_struct['data'].append(i)
                cell_struct['data'].append(reduced[subnode]['props'][cell_yaml['#cells'][1]])

        elif len(cell_yaml['#cells']) == 1:
            for i, cells in enumerate(reduced[subnode]['props']):
                cell_struct['data'].append(reduced[subnode]['props'][cells][0])
                cell_struct['data'].append(reduced[subnode]['props'][cells][1])

    #if 'name' not in prop_struct:
    #    cell_struct['name'].append(cell_yaml['#struct'][0].get('name'))

    prop_struct.append(cell_struct)


  insert_defs(node_address, defs, prop_def, {})
  insert_structs(node_address, 'pinctrl', prop_struct)

def extract_single(node_address, yaml, prop, key, prefix, def_label):

  prop_def = {}
  prop_struct = {}

  prop_struct['data'] = []
  prop_struct['defs'] = {'labels':[], 'aliases':[]}

  if isinstance(prop, list):
    for i, p in enumerate(prop):
      k = convert_string_to_label(key).upper()
      label = def_label + '_' + k
      if isinstance(p, str):
         p = "\"" + p + "\""
      prop_def[label + '_' + str(i)] = p
      #prop_struct[key] = p
      prop_struct['data'].append(p)
      prop_struct['defs']['labels'].append(label + '_' + str(i))
      insert_structs(node_address, key, prop_struct)
  else:
      k = convert_string_to_label(key).upper()
      label = def_label + '_' +  k
      if isinstance(prop, str):
         prop = "\"" + prop + "\""
      prop_def[label] = prop
      #prop_struct[key] = prop
      prop_struct['data'].append(prop)
      prop_struct['defs']['labels'].append(label)
      insert_structs(node_address, key, prop_struct)

  if node_address in defs:
    defs[node_address].update(prop_def)
  else:
    defs[node_address] = prop_def

  return

def extract_property(node_compat, yaml, node_address, y_key, y_val, names, prefix, defs, label_override):

  node = reduced[node_address]

  if 'base_label' in yaml[node_compat]:
    def_label = yaml[node_compat].get('base_label')
  else:
    def_label = convert_string_to_label(node_compat.upper())
    def_label += '_' + node_address.split('@')[-1].upper()

  if label_override is not None:
    def_label += '_' + label_override

  if y_key == 'reg':
    extract_reg_prop(node_address, names, defs, def_label, 1, y_val.get('label', None))
  elif y_key == 'interrupts' or y_key == 'interupts-extended':
    extract_interrupts(node_address, yaml, y_key, names, def_label)
  elif 'pinctrl-' in y_key:
    p_index = int(y_key.split('-')[1])
    extract_pinctrl(node_address, yaml, reduced[node_address]['props'][y_key],
                    names[p_index], p_index, def_label)
  elif 'clocks' in y_key:
    extract_cells(node_address, yaml, y_key,
                  names, 0, prefix, def_label)
  else:
    extract_single(node_address, yaml,
                   reduced[node_address]['props'][y_key], y_key,
                   prefix, def_label)

  return

def extract_node_include_info(reduced, root_node_address, sub_node_address,
                              yaml, defs, structs, y_sub):
  node = reduced[sub_node_address]
  node_compat = get_compat(reduced[root_node_address])
  label_override = None

  if not node_compat in yaml.keys():
    return {}, {}

  if y_sub is None:
    y_node = yaml[node_compat]
  else:
    y_node = y_sub

  if yaml[node_compat].get('use-property-label', False):
    for yp in y_node['properties']:
      if yp.get('label') is not None:
        if node['props'].get('label') is not None:
          label_override = convert_string_to_label(node['props']['label']).upper()
          break

  # check to see if we need to process the properties
  for yp in y_node['properties']:
    for k,v in yp.items():
      if 'properties' in v:
        for c in reduced:
          if root_node_address + '/' in c:
            extract_node_include_info(reduced, root_node_address, c, yaml, defs, structs, v)
      if 'generation' in v:
        if v['generation'] == 'define':
          label = v.get('define_string')
          storage = defs
        else:
          label = v.get('structures_string')
          storage = structs

        prefix = []
        if v.get('use-name-prefix') != None:
          prefix = [convert_string_to_label(k.upper())]


        for c in node['props'].keys():
          if c.endswith("-names"):
            pass

          if re.match(k + '$', c):

            if 'pinctrl-' in c:
              names = node['props'].get('pinctrl-names', [])
            else:
              names = node['props'].get(c[:-1] + '-names', [])
              if not names:
                names = node['props'].get(c + '-names', [])

            if not isinstance(names, list):
              names = [names]

            extract_property(node_compat, yaml, sub_node_address, c, v, names, prefix, defs, label_override)

  return

def yaml_collapse(yaml_list):
  collapsed = dict(yaml_list)

  for k,v in collapsed.items():
    props = set()
    if 'properties' in v:
       for entry in v['properties']:
         for key in entry:
           props.add(key)

    if 'inherits' in v:
      for inherited in v['inherits']:
        for prop in inherited['properties']:
          for key in prop:
            if key not in props:
              v['properties'].append(prop)
      v.pop('inherits')

  return collapsed


def print_key_value(k, v, tabstop):
  label = "#define " + k

  # calculate the name's tabs
  if len(label) % 8:
    tabs = (len(label) + 7)  >> 3
  else:
    tabs = (len(label) >> 3) + 1

  sys.stdout.write(label)
  for i in range(0, tabstop - tabs + 1):
    sys.stdout.write('\t')
  sys.stdout.write(str(v))
  sys.stdout.write("\n")

  return

def generate_keyvalue_file(args):
    compatible = reduced['/']['props']['compatible'][0]

    node_keys = sorted(defs.keys())
    for node in node_keys:
       sys.stdout.write('# ' + node.split('/')[-1] )
       sys.stdout.write("\n")

       prop_keys = sorted(defs[node].keys())
       for prop in prop_keys:
         if prop == 'aliases':
           for entry in sorted(defs[node][prop]):
             a = defs[node][prop].get(entry)
             sys.stdout.write("%s=%s\n" %(entry, defs[node].get(a)))
         else:
           sys.stdout.write("%s=%s\n" %(prop,defs[node].get(prop)))

       sys.stdout.write("\n")

def generate_include_file(args):
    compatible = reduced['/']['props']['compatible'][0]

    sys.stdout.write("/**************************************************\n")
    sys.stdout.write(" * Generated include file for " + compatible)
    sys.stdout.write("\n")
    sys.stdout.write(" *               DO NOT MODIFY\n");
    sys.stdout.write(" */\n")
    sys.stdout.write("\n")
    sys.stdout.write("#ifndef _DEVICE_TREE_BOARD_H" + "\n");
    sys.stdout.write("#define _DEVICE_TREE_BOARD_H" + "\n");
    sys.stdout.write("\n")

    node_keys = sorted(defs.keys())
    for node in node_keys:
       sys.stdout.write('/* ' + node.split('/')[-1] + ' */')
       sys.stdout.write("\n")

       maxlength = max(len(s + '#define ') for s in defs[node].keys())
       if maxlength % 8:
           maxtabstop = (maxlength + 7) >> 3
       else:
         maxtabstop = (maxlength >> 3) + 1

       if (maxtabstop * 8 - maxlength) <= 2:
         maxtabstop += 1

       prop_keys = sorted(defs[node].keys())
       for prop in prop_keys:
         if prop == 'aliases':
           for entry in sorted(defs[node][prop]):
             a = defs[node][prop].get(entry)
             print_key_value(entry, a, maxtabstop)
         else:
           print_key_value(prop, defs[node].get(prop), maxtabstop)

       sys.stdout.write("\n")

    if args.fixup and os.path.exists(args.fixup):
        sys.stdout.write("\n")
        sys.stdout.write("/* Following definitions fixup the generated include */\n")
        try:
            with open(args.fixup, "r") as fd:
                for line in fd.readlines():
                    sys.stdout.write(line)
                sys.stdout.write("\n")
        except:
            raise Exception("Input file " + os.path.abspath(args.fixup) + " does not exist.")

    sys.stdout.write("#endif\n")

def lookup_defs(defs, node, key):
    if node not in defs:
        return None

    if key in defs[node]['aliases']:
        key = defs[node]['aliases'][key]

    return defs[node].get(key, None)

def print_struct_members(node_instance, node, yaml_list, instance_label=0):

    if node in yaml_list:
        node_yaml_props = yaml_list[node]['properties']
        # if 'pinctrl-\\d+' in node_yaml_props.keys():
        #     #rename property in something matching node_instance keys
        #     node_yaml_props['pinctrl'] = node_yaml_props.pop('pinctrl-\\d+')

    for k, v in node_instance[instance_label].items():

        for item in range(0, len(node_yaml_props)):
            if 'pinctrl-\\d+' in node_yaml_props[item].keys():
            #rename property in something matching node_instance keys
                node_yaml_props[item]['pinctrl'] = node_yaml_props[item].pop('pinctrl-\\d+')
            if k in node_yaml_props[item].keys():
                if 'type' in node_yaml_props[item][k].keys():
                    cell_type = node_yaml_props[item][k]['type']

                    if 'array' == cell_type:
                        # sys.stdout.write("\t\t")
                        # sys.stdout.write(str(cell_type) + " ")
                        # sys.stdout.write(str(k) + " " + str(v) + "\n")
                        sys.stdout.write("\t\t")
                        sys.stdout.write("." + convert_string_to_label(k) + " = {")
                        if len(v['members']) > 1:
                            for i in range(0, len(v['data'])):
                                sys.stdout.write("\n\t\t\t\t")
                                sys.stdout.write("." + convert_string_to_label(v['members'][i]) + " = " + str(v['data'][i]) + ",")
                            sys.stdout.write("\n\t\t")
                        else:
                            if len(v['members']) > 0:
                                sys.stdout.write("\n\t\t\t\t ." + convert_string_to_label(str(v['members'])) + " = {")
                            for i in range(0, len(v['data'])):
                                sys.stdout.write(str(v['data'][i]))
                                if i != (len(v['data']) - 1):
                                    sys.stdout.write(" ,")
                            if len(v['members']) > 0:
                                sys.stdout.write("},\n\t\t")
                        sys.stdout.write("},\n")

                    elif 'int' == cell_type:
                        # sys.stdout.write("\t\t")
                        # sys.stdout.write(str(cell_type) + " ")
                        # sys.stdout.write(str(k) + " " + str(v) + "\n")

                        #if isinstance(v['data'], int):
                        if len(v['data']) == 1:
                            sys.stdout.write("\t\t")
                            sys.stdout.write("." + convert_string_to_label(k) + " = " + str(v['data'][0]) + ",\n")
                        else:
                            #If value is array and int expected, print values as int
                            for i in range(0, len(v['data'])):
                                sys.stdout.write("\t\t")
                                sys.stdout.write("." + convert_string_to_label(k) + "_" + str(i) + " = " + str(v['data'][i]) + ",\n")

                    elif 'string' == cell_type:
                        # sys.stdout.write("\t\t")
                        # sys.stdout.write(str(cell_type) + " ")
                        # sys.stdout.write(str(k) + " " + str(v['data']) + "\n")
                        sys.stdout.write("\t\t")
                        sys.stdout.write("." + convert_string_to_label(k) + "[" +  "] = " + str(v['data'][0]) + ",\n")
                    else:
                        raise Exception("Cell type not expected")


def get_member_value(value, node_instances, instance_number):

  prop_rank = 0
  value_data = 0

  value_split = value.split('[')
  prop_name = value_split[0]
  if len(value_split) > 1:
    prop_rank = value_split[1].strip(']')

  node_properties_dict = deepcopy(node_instances[instance_number])

  #prop_name is output of convert_string_to_label
  #convert keys of node_instances[instance_number] dict for latter comparison
  for k in node_properties_dict.keys():
    node_properties_dict[convert_string_to_label(k)] = node_properties_dict.pop(k)

  if prop_name in node_properties_dict.keys():
    value_data = node_properties_dict[prop_name]['data'][int(prop_rank)]

  return value_data

def insert_tab():

  global sub_struct_count
  for i in range(0, sub_struct_count):
    write_node_file("\t")

def open_brace():

  global sub_struct_count

  sub_struct_count += 1
  write_node_file("{\n")

def close_brace():

  global sub_struct_count

  sub_struct_count -= 1
  insert_tab()

  if sub_struct_count > 0:
    write_node_file("},\n")
  else:
    #ending brace
    write_node_file("}")

def flatten_struct(iter, node_irq, node_instances, instance_number):

  iter_dict = {}

  if isinstance(iter, list):
    for i in range(0, len(iter)):
      iter_dict.update(iter[i])
  else:
    iter_dict = iter

  if 'value' in iter_dict.keys():
    cast = ""
    if 'cast' in iter_dict.keys():
       cast = iter_dict['cast']
    if 'interrupts' == iter_dict['value']:
      write_node_file(node_irq['func'] + ",\n")
      if node_irq['flag'] != {}:
        write_node_file("#endif /* " + node_irq['flag'] + " */\n")
    else:
      write_node_file(str(cast) + str(get_member_value(iter_dict['value'], node_instances, instance_number)) + ",\n")
    return

  #not a value, prepare a new sub struct
  open_brace()

  for k, v in iter_dict.items():
    if 'irq_config_func' == k:
      if node_irq['flag'] != {}:
        write_node_file("#ifdef " + node_irq['flag'] + "\n")
      insert_tab()
      write_node_file(".irq_config_func = ")
    else:
      insert_tab()
      write_node_file("." + str(k) + " = ")

    flatten_struct(v, node_irq, node_instances, instance_number)

  close_brace()

def print_driver_init_code(node_instances, node, yaml_list, instance_number=0):

  irq_func= ""

  #for now, don't treat if no label, or no yaml
  if ('label' in node_instances[instance_number]) and (node in yaml_list):
    pass
  else :
    return

  node_yaml_props = yaml_list[node]['properties']

  #get #driver_init struct as dict
  driver_init_dict ={}
  for i in range(0, len(yaml_list[node]['#driver_init'])):
    #if 'irq_config_flag' not in yaml_list[node]['#driver_init'][i].keys():
    driver_init_dict.update(yaml_list[node]['#driver_init'][i])

  #local variables
  node_compat = convert_string_to_label(node)
  instance_label = str(node_instances[instance_number]['label']['data'][0]).strip('"')

  #check in dts if this node has irqs
  node_irq = {}
  if 'interrupts' in node_instances[instance_number]:
    node_irq['data'] = node_instances[instance_number]['interrupts']['data']
    node_irq['func'] = node_compat + "_irq_config_" + instance_label

    if 'irq_config_flag' in driver_init_dict.keys():
      node_irq['flag'] = driver_init_dict['irq_config_flag']
      write_node_file("\n#ifdef " + node_irq['flag'] + "\n")

    write_node_file("static void " + node_irq['func'] + " (struct device * dev);\n")

    if node_irq['flag'] != {}:
      write_node_file("#endif" + "/* " + node_irq['flag'] + " */\n\n")

  # print _data_ / _config_ structs if present
  if len(driver_init_dict.items()) > 0:
    for k, v in driver_init_dict.items():
      if k != 'irq_config_flag':
        write_node_file("\nstatic struct " + node_compat + "_" +  str(k) + "_" + instance_label + " = ")
        flatten_struct(v, node_irq, node_instances, instance_number)
        write_node_file(";\n\n")

  # print DEVICE_AND_API_INIT struct
  write_node_file("DEVICE_AND_API_INIT(" + node_compat + "_dev_" + instance_label + ",\n")
  write_node_file('\t\t    "' + instance_label + '",\n')
  write_node_file('\t\t    &' + node_compat + '_init,\n')
  write_node_file('\t\t    &' + node_compat + '_data_' + instance_label + ',\n')
  write_node_file('\t\t    &' + node_compat + '_config_' + instance_label + ',\n')
  write_node_file('\t\t    PRE_KERNEL_1,\n')
  write_node_file('\t\t    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,\n')
  write_node_file('\t\t    &' + node_compat + '_api);\n')

  # print _irq_func_ if needed
  if node_irq !={}:
    if 'flag' in node_irq.keys():
      write_node_file("\n#ifdef " + node_irq['flag'] + "\n")

    write_node_file("static void " + node_irq['func'] + " (struct device * dev)\n")
    write_node_file("{\n")
    write_node_file("\n")
    for i in range(0, int(len(node_irq['data'])/2)):
      write_node_file("IRQ_CONNECT(" + str(node_irq['data'][2*i]) + " ," + str(node_irq['data'][2*i + 1]) + ",\n")
      if 'interrupts-name' in node_instances[instance_number].keys():
        write_node_file("\t    " + node_compat + "_" + str(node_instances[instance_number]['interrupts-name']['data'][i]).strip('"') + ",\n")
      else:
        write_node_file("\t    " + node_compat + "_isr" + ",\n")
      write_node_file("\t    DEVICE_GET(" + node_compat + '_dev_' + instance_label + "),\n")
      write_node_file("\t    0);\n")
      write_node_file("irq_connect(" + str(node_irq['data'][2*i]) + ");\n\n")
      write_node_file("}\n")
    if 'flag' in node_irq.keys():
      write_node_file("#endif" + "/* " + node_irq['flag'] + " */\n\n")

def write_node_file(str):

  global file
  sys.stdout.write(str)

  if file != "":
    file.write(str)
  else:
    raise Exception("Output file does not exist.")


def generate_structs_file(args, yaml_list):

    global file
    compatible = reduced['/']['props']['compatible'][0]

    #print driver code init
    for node in struct_dict:

        dts_path = str(args.dts).split('/')[:-3]
        outdir_path = '/'.join(dts_path)
        sys.stdout.write("\n")
        if 'zephyr-driver' in yaml_list[node].keys():
          node_init_file_path = str(outdir_path) + '/include/generated/driver/' + str(yaml_list[node]['zephyr-driver'] + '/')
          sys.stdout.write(node_init_file_path + "\n")
        else:
          continue

        node_init_file = node_init_file_path + convert_string_to_label(node) + '_init.c'
        sys.stdout.write("file:" + node_init_file + "\n")

        if not os.path.exists(os.path.dirname(node_init_file)):
          try:
            os.makedirs(os.path.dirname(node_init_file))
          except:
            raise Exception("Could find path: " + node_init_file)

        file = open(node_init_file, 'w')

        sys.stdout.write("\n")

        write_node_file("/**************************************************\n")
        write_node_file(" * Generated include file for " + node)
        write_node_file("\n")
        write_node_file(" *               DO NOT MODIFY\n");
        write_node_file(" */\n")
        write_node_file("\n")
        write_node_file("\n")

        if len(struct_dict[node]) > 1:
            i = 0
            for instance in (struct_dict[node]):

                print_driver_init_code(struct_dict[node], node, yaml_list, i)
                i = i + 1
        else:
            print_driver_init_code(struct_dict[node], node, yaml_list)

        node_init_file = ""
        file.close()

    # TODO: generate pinctrl_struct




def generate_structs(args):


    # Generate structure information here
    #
    # structs structure is:
    # node_address:
    #              prop_0:
    #                     single value -or- list -or- list of dicts
    #              prop_1:
    #                     single value -or- list -or- list of dicts
    #              ...
    #              ...
    # single value: Just a single piece of data (int or string)
    # list: array of int or string
    # list of dicts: array of other structs
    #
    # skip items with None for compat.  These are 'special' (flash, etc)
    # need to run those through chosen node to see if they match and if
    # something should be done.
    #

    # iterate over the structs and reconfigure it to collate by compat
    for k,v in structs.items():
        compat = get_compat(reduced[k])
        if compat is None:
            continue
        if compat not in struct_dict:
            struct_dict[compat] = []
        struct_dict[compat].append(v)

    # now we can process it most efficiently
    pprint.pprint(struct_dict)
    return


def parse_arguments():

  parser = argparse.ArgumentParser(description = __doc__,
                                     formatter_class = argparse.RawDescriptionHelpFormatter)

  parser.add_argument("-d", "--dts", help="DTS file")
  parser.add_argument("-y", "--yaml", help="YAML file")
  parser.add_argument("-s", "--structs", action="store_true")
  parser.add_argument("-f", "--fixup", help="Fixup file")
  parser.add_argument("-k", "--keyvalue", action="store_true",
          help="Generate file to be included by the build system")

  return parser.parse_args()

def main():
  args = parse_arguments()
  if not args.dts or not args.yaml:
    print('Usage: %s -d filename.dts -y path_to_yaml' % sys.argv[0])
    return 1

  try:
    with open(args.dts, "r") as fd:
      d = parse_file(fd)
  except:
     raise Exception("Input file " + os.path.abspath(args.dts) + " does not exist.")

  # compress list to nodes w/ paths, add interrupt parent
  compress_nodes(d['/'], '/')

  # build up useful lists
  compatibles = get_all_compatibles(d['/'], '/', {})
  get_phandles(d['/'], '/', {})
  get_aliases(d['/'])
  get_chosen(d['/'])

  # find unique set of compatibles across all active nodes
  s = set()
  for k,v in compatibles.items():
    if isinstance(v,list):
      for item in v:
        s.add(item)
    else:
      s.add(v)

  # scan YAML files and find the ones we are interested in
  yaml_files = []
  for filename in listdir(args.yaml):
    if re.match('.*\.yaml\Z', filename):
      yaml_files.append(os.path.realpath(args.yaml + '/' + filename))

  # scan common YAML files and find the ones we are interested in
  zephyrbase = os.environ.get('ZEPHYR_BASE')
  if zephyrbase is not None:
    for filename in listdir(zephyrbase + '/dts/common/yaml'):
      if re.match('.*\.yaml\Z', filename):
        yaml_files.append(os.path.realpath(zephyrbase + '/dts/common/yaml/' + filename))

  yaml_list = {}
  file_load_list = set()
  for file in yaml_files:
    for line in open(file, 'r'):
      if re.search('^\s+constraint:*', line):
        c = line.split(':')[1].strip()
        c = c.strip('"')
        if c in s:
          if not file in file_load_list:
            file_load_list.add(file)
            with open(file, 'r') as yf:
              yaml_list[c] = yaml.load(yf, Loader)

  if yaml_list == {}:
    raise Exception("Missing YAML information.  Check YAML sources")

  # collapse the yaml inherited information
  yaml_list = yaml_collapse(yaml_list)

#  defs = {}
  structs = {}
  # load zephyr specific nodes
  flash = {}
  console = {}
  sram = {}
  if 'zephyr,flash' in chosen:
    flash = reduced[chosen['zephyr,flash']]
  if 'zephyr,console' in chosen:
    console = reduced[chosen['zephyr,console']]
  if 'zephyr,sram' in chosen:
    sram = reduced[chosen['zephyr,sram']]
  for k, v in reduced.items():
    node_compat = get_compat(v)
    if node_compat != None and node_compat in yaml_list:
      extract_node_include_info(reduced, k, k, yaml_list, defs, structs, None)

  if defs == {}:
    raise Exception("No information parsed from dts file.")

  if 'zephyr,flash' in chosen:
    extract_reg_prop(chosen['zephyr,flash'], None, defs, "CONFIG_FLASH", 1024, None)
  else:
    # We will add address and size of 0 for systems with no flash controller
    # This is what they already do in the Kconfig options anyway
    defs['dummy-flash'] =  { 'CONFIG_FLASH_BASE_ADDRESS': 0, 'CONFIG_FLASH_SIZE': 0 }

  if 'zephyr,sram' in chosen:
    extract_reg_prop(chosen['zephyr,sram'], None, defs, "CONFIG_SRAM", 1024, None)

  # only compute the load offset if a code partition exists and it is not the
  # same as the flash base address
  load_defs = {}
  if 'zephyr,code-partition' in chosen and \
     'zephyr,flash' in chosen and \
     reduced[chosen['zephyr,flash']] is not \
              reduced[chosen['zephyr,code-partition']]:
    flash_base = lookup_defs(defs, chosen['zephyr,flash'],
                              'CONFIG_FLASH_BASE_ADDRESS')
    part_defs = {}
    extract_reg_prop(chosen['zephyr,code-partition'], None, part_defs,
                     "PARTITION", 1, 'offset')
    part_base = lookup_defs(part_defs, chosen['zephyr,code-partition'],
                     'PARTITION_OFFSET')
    load_defs['CONFIG_FLASH_LOAD_OFFSET'] = part_base
    load_defs['CONFIG_FLASH_LOAD_SIZE'] = \
            lookup_defs(part_defs, chosen['zephyr,code-partition'],
                        'PARTITION_SIZE')
  else:
    load_defs['CONFIG_FLASH_LOAD_OFFSET'] = 0
    load_defs['CONFIG_FLASH_LOAD_SIZE'] = 0

  insert_defs(chosen['zephyr,flash'], defs, load_defs, {})

  pprint.pprint(defs)
  pprint.pprint(structs)
  #generate include file
  if args.keyvalue:
   generate_keyvalue_file(args)
  elif args.structs:
   generate_structs(args)
   generate_structs_file(args, yaml_list)
  else:
   generate_include_file(args)

if __name__ == '__main__':
    main()
