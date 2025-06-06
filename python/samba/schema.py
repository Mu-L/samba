#
# Unix SMB/CIFS implementation.
# backend code for provisioning a Samba4 server
#
# Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007-2008
# Copyright (C) Andrew Bartlett <abartlet@samba.org> 2008-2009
# Copyright (C) Oliver Liebel <oliver@itc.li> 2008-2009
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

"""Functions for setting up a Samba Schema."""

from base64 import b64encode
from samba import read_and_sub_file, substitute_var, check_all_substituted
from samba.dcerpc import security
from samba.ms_schema import read_ms_schema
from samba.ndr import ndr_pack
from samba.samdb import SamDB
from samba.common import get_string
from samba import dsdb
from ldb import OID_COMPARATOR_AND, SCOPE_SUBTREE, SCOPE_ONELEVEL


def get_schema_descriptor(domain_sid, name_map=None):
    if name_map is None:
        name_map = {}

    sddl = "O:SAG:SAD:AI(OA;;CR;e12b56b6-0a95-11d1-adbb-00c04fd8d5cd;;SA)" \
           "(OA;;CR;1131f6aa-9c07-11d1-f79f-00c04fc2dcd2;;ED)" \
           "(OA;;CR;1131f6ab-9c07-11d1-f79f-00c04fc2dcd2;;ED)" \
           "(OA;;CR;1131f6ac-9c07-11d1-f79f-00c04fc2dcd2;;ED)" \
           "(OA;;CR;1131f6aa-9c07-11d1-f79f-00c04fc2dcd2;;BA)" \
           "(OA;;CR;1131f6ab-9c07-11d1-f79f-00c04fc2dcd2;;BA)" \
           "(OA;;CR;1131f6ac-9c07-11d1-f79f-00c04fc2dcd2;;BA)" \
           "(A;CI;RPLCLORC;;;AU)" \
           "(A;CI;RPWPCRCCLCLORCWOWDSW;;;SA)" \
           "(A;CI;RPWPCRCCDCLCLORCWOWDSDDTSW;;;SY)" \
           "(OA;;CR;1131f6ad-9c07-11d1-f79f-00c04fc2dcd2;;ED)" \
           "(OA;;CR;89e95b76-444d-4c62-991a-0facbeda640c;;ED)" \
           "(OA;;CR;1131f6ad-9c07-11d1-f79f-00c04fc2dcd2;;BA)" \
           "(OA;;CR;89e95b76-444d-4c62-991a-0facbeda640c;;BA)" \
           "(OA;;CR;1131f6aa-9c07-11d1-f79f-00c04fc2dcd2;;RO)" \
           "(OA;;CR;1131f6ad-9c07-11d1-f79f-00c04fc2dcd2;;RO)" \
           "(OA;;CR;89e95b76-444d-4c62-991a-0facbeda640c;;RO)" \
           "S:(AU;SA;WPCCDCWOWDSDDTSW;;;WD)" \
           "(AU;CISA;WP;;;WD)" \
           "(AU;SA;CR;;;BA)" \
           "(AU;SA;CR;;;DU)" \
           "(OU;SA;CR;e12b56b6-0a95-11d1-adbb-00c04fd8d5cd;;WD)" \
           "(OU;SA;CR;45ec5156-db7e-47bb-b53f-dbeb2d03c40f;;WD)"
    sec = security.descriptor.from_sddl(sddl, domain_sid)
    return ndr_pack(sec)


class Schema(object):

    # the schema files (and corresponding object version) that we know about
    base_schemas = {
        "2008_R2_old": ("MS-AD_Schema_2K8_R2_Attributes.txt",
                        "MS-AD_Schema_2K8_R2_Classes.txt",
                        47),
       "2008_R2": ("Attributes_for_AD_DS__Windows_Server_2008_R2.ldf",
                   "Classes_for_AD_DS__Windows_Server_2008_R2.ldf",
                   47),
       "2012": ("Attributes_for_AD_DS__Windows_Server_2012.ldf",
                "Classes_for_AD_DS__Windows_Server_2012.ldf",
                56),
       "2012_R2": ("AD_DS_Attributes__Windows_Server_2012_R2.ldf",
                   "AD_DS_Classes__Windows_Server_2012_R2.ldf",
                   69),
       "2016": ("AD_DS_Attributes__Windows_Server_v1803.ldf",
                "AD_DS_Classes__Windows_Server_v1803.ldf",
                87),
       "2019": ("AD_DS_Attributes_Windows_Server_v1903.ldf",
                "AD_DS_Classes_Windows_Server_v1903.ldf",
                88),
    }

    def __init__(self, domain_sid, invocationid=None, schemadn=None,
                 files=None, override_prefixmap=None, additional_prefixmap=None,
                 base_schema=None):
        from samba.provision import setup_path

        """Load schema for the SamDB from the AD schema files and
        samba4_schema.ldif

        :param samdb: Load a schema into a SamDB.
        :param schemadn: DN of the schema

        Returns the schema data loaded, to avoid double-parsing when then
        needing to add it to the db
        """

        if base_schema is None:
            base_schema = Schema.default_base_schema()

        self.base_schema = base_schema

        self.schemadn = schemadn
        # We need to have the am_rodc=False just to keep some warnings quiet -
        # this isn't a real SAM, so it's meaningless.
        self.ldb = SamDB(global_schema=False, am_rodc=False)
        if invocationid is not None:
            self.ldb.set_invocation_id(invocationid)

        self.schema_data = read_ms_schema(
            setup_path('ad-schema/%s' % Schema.base_schemas[base_schema][0]),
            setup_path('ad-schema/%s' % Schema.base_schemas[base_schema][1]))

        def read_file(file):
            with open(file, 'rb') as data_file:
                return data_file.read()

        if files is not None:
            self.schema_data = "".join(get_string(read_file(file))
                                       for file in files)

        self.schema_data = substitute_var(self.schema_data,
                                          {"SCHEMADN": schemadn})
        check_all_substituted(self.schema_data)

        schema_version = str(Schema.get_version(base_schema))
        self.schema_dn_modify = read_and_sub_file(
            setup_path("provision_schema_basedn_modify.ldif"),
            {"SCHEMADN": schemadn, "OBJVERSION": schema_version})

        descr = b64encode(get_schema_descriptor(domain_sid)).decode('utf8')
        self.schema_dn_add = read_and_sub_file(
            setup_path("provision_schema_basedn.ldif"),
            {"SCHEMADN": schemadn, "DESCRIPTOR": descr})

        if override_prefixmap is not None:
            self.prefixmap_data = override_prefixmap
        else:
            self.prefixmap_data = read_file(setup_path("prefixMap.txt"))

        if additional_prefixmap is not None:
            self.prefixmap_data += "".join("%s\n" % map for map in additional_prefixmap)

        self.prefixmap_data = b64encode(self.prefixmap_data).decode('utf8')

        # We don't actually add this ldif, just parse it
        prefixmap_ldif = "dn: %s\nprefixMap:: %s\n\n" % (self.schemadn, self.prefixmap_data)
        self.set_from_ldif(prefixmap_ldif, self.schema_data, self.schemadn)

    @staticmethod
    def default_base_schema():
        """Returns the default base schema to use"""
        return "2012_R2"

    @staticmethod
    def get_version(base_schema):
        """Returns the base schema's object version, e.g. 47 for 2008_R2"""
        return Schema.base_schemas[base_schema][2]

    def set_from_ldif(self, pf, df, dn):
        dsdb._dsdb_set_schema_from_ldif(self.ldb, pf, df, dn)

    def write_to_tmp_ldb(self, schemadb_path):
        self.ldb.connect(url=schemadb_path)
        self.ldb.transaction_start()
        try:
            # These are actually ignored, as the schema has been forced
            # when the ldb object was created, and that overrides this
            self.ldb.add_ldif("""dn: @ATTRIBUTES
linkID: INTEGER

dn: @INDEXLIST
@IDXATTR: linkID
@IDXATTR: attributeSyntax
@IDXGUID: objectGUID
""")

            schema_dn_add = self.schema_dn_add \
                + "objectGUID: 24e2ca70-b093-4ae8-84c0-2d7ac652a1b8\n"

            # These bits of LDIF are supplied when the Schema object is created
            self.ldb.add_ldif(schema_dn_add)
            self.ldb.modify_ldif(self.schema_dn_modify)
            self.ldb.add_ldif(self.schema_data)
        except:
            self.ldb.transaction_cancel()
            raise
        else:
            self.ldb.transaction_commit()

    # Return a hash with the forward attribute as a key and the back as the
    # value
    def linked_attributes(self):
        return get_linked_attributes(self.schemadn, self.ldb)

    def dnsyntax_attributes(self):
        return get_dnsyntax_attributes(self.schemadn, self.ldb)

    def convert_to_openldap(self, target, mapping):
        return dsdb._dsdb_convert_schema_to_openldap(self.ldb, target, mapping)


# Return a hash with the forward attribute as a key and the back as the value
def get_linked_attributes(schemadn, schemaldb):
    attrs = ["linkID", "lDAPDisplayName"]
    res = schemaldb.search(
        expression="(&(linkID=*)"
                   f"(!(linkID:{OID_COMPARATOR_AND}:=1))"
                   "(objectclass=attributeSchema)"
                   "(attributeSyntax=2.5.5.1))",
        base=schemadn, scope=SCOPE_ONELEVEL, attrs=attrs)
    attributes = {}
    for i in range(0, len(res)):
        expression = ("(&(objectclass=attributeSchema)(linkID=%d)"
                      "(attributeSyntax=2.5.5.1))" %
                      (int(res[i]["linkID"][0]) + 1))
        target = schemaldb.searchone(basedn=schemadn,
                                     expression=expression,
                                     attribute="lDAPDisplayName",
                                     scope=SCOPE_SUBTREE)
        if target is not None:
            attributes[str(res[i]["lDAPDisplayName"])] = target.decode('utf-8')

    return attributes


def get_dnsyntax_attributes(schemadn, schemaldb):
    res = schemaldb.search(
        expression="(&(!(linkID=*))(objectclass=attributeSchema)(attributeSyntax=2.5.5.1))",
        base=schemadn, scope=SCOPE_ONELEVEL,
        attrs=["linkID", "lDAPDisplayName"])
    attributes = []
    for i in range(0, len(res)):
        attributes.append(str(res[i]["lDAPDisplayName"]))
    return attributes


def ldb_with_schema(schemadn="cn=schema,cn=configuration,dc=example,dc=com",
                    domainsid=None,
                    override_prefixmap=None):
    """Load schema for the SamDB from the AD schema files and samba4_schema.ldif

    :param schemadn: DN of the schema
    :param serverdn: DN of the server

    Returns the schema data loaded as an object, with .ldb being a
    new ldb with the schema loaded.  This allows certain tests to
    operate without a remote or local schema.
    """

    if domainsid is None:
        domainsid = security.random_sid()
    else:
        domainsid = security.dom_sid(domainsid)
    return Schema(domainsid, schemadn=schemadn,
                  override_prefixmap=override_prefixmap)
