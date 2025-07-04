# Unix SMB/CIFS implementation.
# Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007-2010
# Copyright (C) Matthias Dieter Wallnoefer 2009
#
# Based on the original in EJS:
# Copyright (C) Andrew Tridgell <tridge@samba.org> 2005
# Copyright (C) Giampaolo Lauria <lauria2@yahoo.com> 2011
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

"""Convenience functions for using the SAM."""

import samba
import ldb
import time
import base64
import os
import re
from samba import dsdb, dsdb_dns
from samba.ndr import ndr_unpack, ndr_pack
from samba.dcerpc import drsblobs, misc
from samba.common import normalise_int32
from samba.common import get_bytes, cmp
from samba.dcerpc import security
from samba import is_ad_dc_built
from samba import string_is_guid
from samba import NTSTATUSError, ntstatus
import binascii

__docformat__ = "restructuredText"


def get_default_backend_store():
    return "tdb"

class SamDBError(Exception):
    pass

class SamDBNotFoundError(SamDBError):
    pass

class SamDB(samba.Ldb):
    """The SAM database."""

    hash_oid_name = {}

    class _CleanUpOnError:
        def __init__(self, samdb, dn):
            self.samdb = samdb
            self.dn = dn

        def __enter__(self):
            pass

        def __exit__(self, exc_type, exc_val, exc_tb):
            if exc_type is not None:
                # We failed to modify the account. If we connected to the
                # database over LDAP, we don't have transactions, and so when
                # we call transaction_cancel(), the account will still exist in
                # a half-created state. We'll delete the account to ensure that
                # doesn't happen.
                self.samdb.delete(self.dn)

            # Don't suppress any exceptions
            return False

    def __init__(self, url=None, lp=None, modules_dir=None, session_info=None,
                 credentials=None, flags=ldb.FLG_DONT_CREATE_DB,
                 options=None, global_schema=True,
                 auto_connect=True, am_rodc=None):
        self.lp = lp
        if not auto_connect:
            url = None
        elif url is None and lp is not None:
            url = lp.samdb_url()

        self.url = url

        super().__init__(url=url, lp=lp, modules_dir=modules_dir,
                         session_info=session_info, credentials=credentials, flags=flags,
                         options=options)

        if global_schema:
            dsdb._dsdb_set_global_schema(self)

        if am_rodc is not None:
            dsdb._dsdb_set_am_rodc(self, am_rodc)

    def connect(self, url=None, flags=0, options=None):
        """connect to the database"""
        if self.lp is not None and not os.path.exists(url):
            url = self.lp.private_path(url)
        self.url = url

        super().connect(url=url, flags=flags, options=options)

    def __repr__(self):
        if self.url:
            return f"<SamDB {id(self):x} ({self.url})>"

        return f"<SamDB {id(self):x} (no connection)>"

    __str__ = __repr__

    def am_rodc(self):
        """return True if we are an RODC"""
        return dsdb._am_rodc(self)

    def am_pdc(self):
        """return True if we are an PDC emulator"""
        return dsdb._am_pdc(self)

    def domain_dn(self):
        """return the domain DN"""
        return str(self.get_default_basedn())

    def schema_dn(self):
        """return the schema partition dn"""
        return str(self.get_schema_basedn())

    def disable_account(self, search_filter):
        """Disables an account

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        """

        flags = samba.dsdb.UF_ACCOUNTDISABLE
        self.toggle_userAccountFlags(search_filter, flags, on=True)

    def enable_account(self, search_filter):
        """Enables an account

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        """

        flags = samba.dsdb.UF_ACCOUNTDISABLE | samba.dsdb.UF_PASSWD_NOTREQD
        self.toggle_userAccountFlags(search_filter, flags, on=False)

    def toggle_userAccountFlags(self, search_filter, flags, flags_str=None,
                                on=True, strict=False):
        """Toggle_userAccountFlags

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        :param flags: samba.dsdb.UF_* flags
        :param on: on=True (default) => set, on=False => unset
        :param strict: strict=False (default) ignore if no action is needed
                 strict=True raises an Exception if...
        """
        res = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                          expression=search_filter, attrs=["userAccountControl"])
        if len(res) == 0:
                raise Exception("Unable to find account where '%s'" % search_filter)
        assert(len(res) == 1)
        account_dn = res[0].dn

        old_uac = int(res[0]["userAccountControl"][0])
        if on:
            if strict and (old_uac & flags):
                error = "Account flag(s) '%s' already set" % flags_str
                raise Exception(error)

            new_uac = old_uac | flags
        else:
            if strict and not (old_uac & flags):
                error = "Account flag(s) '%s' already unset" % flags_str
                raise Exception(error)

            new_uac = old_uac & ~flags

        if old_uac == new_uac:
            return

        mod = """
dn: %s
changetype: modify
delete: userAccountControl
userAccountControl: %u
add: userAccountControl
userAccountControl: %u
""" % (account_dn, old_uac, new_uac)
        self.modify_ldif(mod)

    def force_password_change_at_next_login(self, search_filter):
        """Forces a password change at next login

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        """
        res = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                          expression=search_filter, attrs=[])
        if len(res) == 0:
                raise Exception('Unable to find user "%s"' % search_filter)
        assert(len(res) == 1)
        user_dn = res[0].dn

        mod = """
dn: %s
changetype: modify
replace: pwdLastSet
pwdLastSet: 0
""" % (user_dn)
        self.modify_ldif(mod)

    def unlock_account(self, search_filter):
        """Unlock a user account by resetting lockoutTime to 0.
        This does also reset the badPwdCount to 0.

        :param search_filter: LDAP filter to find the user (e.g.
            sAMAccountName=username)
        """
        res = self.search(base=self.domain_dn(),
                          scope=ldb.SCOPE_SUBTREE,
                          expression=search_filter,
                          attrs=[])
        if len(res) == 0:
            raise SamDBNotFoundError('Unable to find user "%s"' % search_filter)
        if len(res) != 1:
            raise SamDBError('User "%s" is not unique' % search_filter)
        user_dn = res[0].dn

        mod = """
dn: %s
changetype: modify
replace: lockoutTime
lockoutTime: 0
""" % (user_dn)
        self.modify_ldif(mod)

    def newgroup(self, groupname, groupou=None, grouptype=None,
                 description=None, mailaddress=None, notes=None, sd=None,
                 gidnumber=None, nisdomain=None):
        """Adds a new group with additional parameters

        :param groupname: Name of the new group
        :param grouptype: Type of the new group
        :param description: Description of the new group
        :param mailaddress: Email address of the new group
        :param notes: Notes of the new group
        :param gidnumber: GID Number of the new group
        :param nisdomain: NIS Domain Name of the new group
        :param sd: security descriptor of the object
        """

        if groupou:
            group_dn = "CN=%s,%s,%s" % (groupname, groupou, self.domain_dn())
        else:
            group_dn = "CN=%s,%s" % (groupname, self.get_wellknown_dn(
                                        self.get_default_basedn(),
                                        dsdb.DS_GUID_USERS_CONTAINER))

        # The new user record. Note the reliance on the SAMLDB module which
        # fills in the default information
        ldbmessage = {"dn": group_dn,
                      "sAMAccountName": groupname,
                      "objectClass": "group"}

        if grouptype is not None:
            ldbmessage["groupType"] = normalise_int32(grouptype)

        if description is not None:
            ldbmessage["description"] = description

        if mailaddress is not None:
            ldbmessage["mail"] = mailaddress

        if notes is not None:
            ldbmessage["info"] = notes

        if gidnumber is not None:
            ldbmessage["gidNumber"] = normalise_int32(gidnumber)

        if nisdomain is not None:
            ldbmessage["msSFU30Name"] = groupname
            ldbmessage["msSFU30NisDomain"] = nisdomain

        if sd is not None:
            ldbmessage["nTSecurityDescriptor"] = ndr_pack(sd)

        self.add(ldbmessage)

    def deletegroup(self, groupname):
        """Deletes a group

        :param groupname: Name of the target group
        """

        groupfilter = "(&(sAMAccountName=%s)(objectCategory=%s,%s))" % (ldb.binary_encode(groupname), "CN=Group,CN=Schema,CN=Configuration", self.domain_dn())
        self.transaction_start()
        try:
            targetgroup = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                                      expression=groupfilter, attrs=[])
            if len(targetgroup) == 0:
                raise Exception('Unable to find group "%s"' % groupname)
            assert(len(targetgroup) == 1)
            self.delete(targetgroup[0].dn)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def group_member_filter(self, member, member_types):
        filter = ""

        all_member_types = [ 'user',
                             'group',
                             'computer',
                             'serviceaccount',
                             'contact',
                           ]

        if 'all' in member_types:
            member_types = all_member_types

        for member_type in member_types:
            if member_type not in all_member_types:
                raise Exception('Invalid group member type "%s". '
                                'Valid types are %s and all.' %
                                (member_type, ", ".join(all_member_types)))

        if 'user' in member_types:
            filter += ('(&(sAMAccountName=%s)(samAccountType=%d))' %
                       (ldb.binary_encode(member), dsdb.ATYPE_NORMAL_ACCOUNT))
        if 'group' in member_types:
            filter += ('(&(sAMAccountName=%s)'
                       '(objectClass=group)'
                       f'(!(groupType:{ldb.OID_COMPARATOR_AND}:=1)))' %
                       ldb.binary_encode(member))
        if 'computer' in member_types:
            samaccountname = member
            if member[-1] != '$':
                samaccountname = "%s$" % member
            filter += ('(&(samAccountType=%d)'
                       '(!(objectCategory=msDS-ManagedServiceAccount))'
                       '(sAMAccountName=%s))' %
                       (dsdb.ATYPE_WORKSTATION_TRUST,
                        ldb.binary_encode(samaccountname)))
        if 'serviceaccount' in member_types:
            samaccountname = member
            if member[-1] != '$':
                samaccountname = "%s$" % member
            filter += ('(&(samAccountType=%d)'
                       '(objectCategory=msDS-ManagedServiceAccount)'
                       '(sAMAccountName=%s))' %
                       (dsdb.ATYPE_WORKSTATION_TRUST,
                        ldb.binary_encode(samaccountname)))
        if 'contact' in member_types:
            filter += ('(&(objectCategory=Person)(!(objectSid=*))(name=%s))' %
                       ldb.binary_encode(member))

        filter = "(|%s)" % filter

        return filter

    def add_remove_group_members(self, group, members,
                                 add_members_operation=True,
                                 member_types=None,
                                 member_base_dn=None):
        """Adds or removes group members

        :param group: sAMAccountName, DN, SID or GUID of the target group
        :param members: list of group members
        :param add_members_operation: Defines if its an add or remove
            operation
        :param member_types: List of object types, used to filter the search
            for the specified members
        :param member_base_dn: Base dn for member search
        """
        if member_types is None:
            member_types = ['user', 'group', 'computer']

        if member_base_dn is None:
            member_base_dn = self.domain_dn()

        partial_groupfilter = None

        # If <group> looks like a SID, GUID, or DN, we use it
        # accordingly, otherwise as a name.
        #
        # Because misc.GUID() will read any 16 byte sequence as a
        # binary guid, we need to be careful not to read 16 character
        # names as GUIDs.

        group_sid = None
        try:
            group_sid = security.dom_sid(group)
        except ValueError:
            pass
        if group_sid is not None:
            partial_groupfilter = "(objectClass=*)"

        group_guid = None
        if partial_groupfilter is None and string_is_guid(group):
            try:
                group_guid = misc.GUID(group)
            except NTSTATUSError as e:
                (status, _) = e.args
                if status != ntstatus.NT_STATUS_INVALID_PARAMETER:
                    raise e
            if group_guid is not None:
                partial_groupfilter = "(objectClass=*)"

        if partial_groupfilter is None:
            group_dn = None
            try:
                if isinstance(group, ldb.Dn):
                    group_dn = ldb.Dn(self, group.extended_str(1))
                else:
                    group_dn = ldb.Dn(self, str(group))
            except ValueError:
                pass
            if group_dn is not None:
                group_b_sid = group_dn.get_extended_component("SID")
                group_b_guid = group_dn.get_extended_component("GUID")
                if group_b_sid is not None:
                    group_sid = ndr_unpack(security.dom_sid, group_b_sid)
                    partial_groupfilter = "(objectClass=*)"
                elif group_b_guid is not None:
                    group_guid = ndr_unpack(misc.GUID, group_b_guid)
                    partial_groupfilter = "(objectClass=*)"
                else:
                    search_base = str(group_dn)
                    search_scope = ldb.SCOPE_BASE

        if group_sid is not None:
            search_base = '<SID=%s>' % group_sid
            search_scope = ldb.SCOPE_BASE

        if group_guid is not None:
            search_base = '<GUID=%s>' % group_guid
            search_scope = ldb.SCOPE_BASE

        if partial_groupfilter is None:
            search_base = self.domain_dn()
            search_scope = ldb.SCOPE_SUBTREE
            partial_groupfilter = "(sAMAccountName=%s)" % (
                ldb.binary_encode(group))

        groupfilter = "(&%s(objectCategory=%s,%s))" % (
            partial_groupfilter,
            "CN=Group,CN=Schema,CN=Configuration",
            self.domain_dn())

        self.transaction_start()
        try:
            targetgroup = self.search(base=search_base,
                                      scope=search_scope,
                                      expression=groupfilter,
                                      controls=["extended_dn:1:1"],
                                      attrs=['member'])
            if len(targetgroup) == 0:
                raise Exception('Unable to find group "%s"' % group)
            assert(len(targetgroup) == 1)

            modified = False

            if group_sid is not None:
                targetgroup_dn = '<SID=%s>' % group_sid
            elif group_guid is not None:
                targetgroup_dn = '<GUID=%s>' % group_guid
            else:
                targetgroup_dn = str(targetgroup[0].dn)

            addtargettogroup = """
dn: %s
changetype: modify
""" % (targetgroup_dn)

            for member in members:
                targetmember_dn = None
                membersid = None
                try:
                    membersid = security.dom_sid(member)
                    targetmember_dn = "<SID=%s>" % str(membersid)
                except ValueError:
                    pass

                if targetmember_dn is None:
                    try:
                        member_dn = ldb.Dn(self, member)
                        if member_dn.get_linearized() == member_dn.extended_str(1):
                            full_member_dn = self.normalize_dn_in_domain(member_dn)
                        else:
                            full_member_dn = member_dn
                        targetmember_dn = full_member_dn.extended_str(1)
                    except ValueError as e:
                        pass

                if targetmember_dn is None:
                    search_filter = self.group_member_filter(member, member_types)
                    targetmember = self.search(base=member_base_dn,
                                               scope=ldb.SCOPE_SUBTREE,
                                               expression=search_filter,
                                               attrs=[])

                    if len(targetmember) > 1:
                        targetmemberlist_str = ""
                        for msg in targetmember:
                            targetmemberlist_str += "%s\n" % msg.get("dn")
                        raise Exception('Found multiple results for "%s":\n%s' %
                                        (member, targetmemberlist_str))
                    if len(targetmember) != 1:
                        raise Exception('Unable to find "%s". Operation cancelled.' % member)
                    targetmember_dn = targetmember[0].dn.extended_str(1)

                def _is_member(samdb, group, member_dn, member_sid):
                    if group.get('member') is None:
                        return False

                    for m in group.get('member'):
                        m_ext_dn = ldb.Dn(samdb, str(m))
                        m_binary_sid = m_ext_dn.get_extended_component("SID")
                        if m_binary_sid:
                            m_sid = ndr_unpack(security.dom_sid, m_binary_sid)
                            if member_sid == m_sid:
                                return True
                        if member_dn == str(m_ext_dn):
                            return True

                    return False

                is_member = _is_member(self,
                                       targetgroup[0],
                                       targetmember_dn,
                                       membersid)
                if add_members_operation is True and not is_member:
                    modified = True
                    addtargettogroup += """add: member
member: %s
""" % (str(targetmember_dn))

                elif add_members_operation is False and is_member:
                    modified = True
                    addtargettogroup += """delete: member
member: %s
""" % (str(targetmember_dn))

            if modified is True:
                self.modify_ldif(addtargettogroup)

        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def prepare_attr_replace(self, msg, old, attr_name, value):
        """Changes the MessageElement with the given attr_name of the
        given Message. If the value is "" set an empty value and the flag
        FLAG_MOD_DELETE, otherwise set the new value and FLAG_MOD_REPLACE.
        If the value is None or the Message contains the attr_name with this
        value, nothing will changed."""
        # skip unchanged attribute
        if value is None:
            return
        if attr_name in old and str(value) == str(old[attr_name]):
            return

        # remove attribute
        if len(value) == 0:
            if attr_name in old:
                el = ldb.MessageElement([], ldb.FLAG_MOD_DELETE, attr_name)
                msg.add(el)
            return

        # change attribute
        el = ldb.MessageElement(value, ldb.FLAG_MOD_REPLACE, attr_name)
        msg.add(el)

    def fullname_from_names(self, given_name=None, initials=None, surname=None,
                            old_attrs=None, fallback_default=""):
        """Prepares new combined fullname, using the name parts.
        Used for things like displayName or cn.
        Use the original name values, if no new one is specified."""
        if old_attrs is None:
            old_attrs = {}

        attrs = {"givenName": given_name,
                 "initials": initials,
                 "sn": surname}

        # if the attribute is not specified, try to use the old one
        for attr_name, attr_value in attrs.items():
            if attr_value is None and attr_name in old_attrs:
                attrs[attr_name] = str(old_attrs[attr_name])

        # add '.' to initials if initials are not None and not "" and if the initials
        # don't have already a '.' at the end
        if attrs["initials"] and not attrs["initials"].endswith('.'):
            attrs["initials"] += '.'

        # remove empty values (None and '')
        attrs_values = list(filter(None, attrs.values()))

        # fullname is the combination of not-empty values as string, separated by ' '
        fullname = ' '.join(attrs_values)

        if fullname == '':
            return fallback_default

        return fullname

    def newuser(self, username, password,
                force_password_change_at_next_login_req=False,
                useusernameascn=False, userou=None, surname=None, givenname=None,
                initials=None, profilepath=None, scriptpath=None, homedrive=None,
                homedirectory=None, jobtitle=None, department=None, company=None,
                description=None, mailaddress=None, internetaddress=None,
                telephonenumber=None, physicaldeliveryoffice=None, sd=None,
                setpassword=True, uidnumber=None, gidnumber=None, gecos=None,
                loginshell=None, uid=None, nisdomain=None, unixhome=None,
                smartcard_required=False):
        """Adds a new user with additional parameters

        :param username: Name of the new user
        :param password: Password for the new user
        :param force_password_change_at_next_login_req: Force password change
        :param useusernameascn: Use username as cn rather that firstname +
            initials + lastname
        :param userou: Object container (without domainDN postfix) for new user
        :param surname: Surname of the new user
        :param givenname: First name of the new user
        :param initials: Initials of the new user
        :param profilepath: Profile path of the new user
        :param scriptpath: Logon script path of the new user
        :param homedrive: Home drive of the new user
        :param homedirectory: Home directory of the new user
        :param jobtitle: Job title of the new user
        :param department: Department of the new user
        :param company: Company of the new user
        :param description: of the new user
        :param mailaddress: Email address of the new user
        :param internetaddress: Home page of the new user
        :param telephonenumber: Phone number of the new user
        :param physicaldeliveryoffice: Office location of the new user
        :param sd: security descriptor of the object
        :param setpassword: optionally disable password reset
        :param uidnumber: RFC2307 Unix numeric UID of the new user
        :param gidnumber: RFC2307 Unix primary GID of the new user
        :param gecos: RFC2307 Unix GECOS field of the new user
        :param loginshell: RFC2307 Unix login shell of the new user
        :param uid: RFC2307 Unix username of the new user
        :param nisdomain: RFC2307 Unix NIS domain of the new user
        :param unixhome: RFC2307 Unix home directory of the new user
        :param smartcard_required: set the UF_SMARTCARD_REQUIRED bit of the new user
        """

        displayname = self.fullname_from_names(given_name=givenname,
                                               initials=initials,
                                               surname=surname)
        cn = username
        if useusernameascn is None and displayname != "":
            cn = displayname

        if userou:
            user_dn = "CN=%s,%s,%s" % (cn, userou, self.domain_dn())
        else:
            user_dn = "CN=%s,%s" % (cn, self.get_wellknown_dn(
                                        self.get_default_basedn(),
                                        dsdb.DS_GUID_USERS_CONTAINER))

        dnsdomain = ldb.Dn(self, self.domain_dn()).canonical_str().replace("/", "")
        user_principal_name = "%s@%s" % (username, dnsdomain)
        # The new user record. Note the reliance on the SAMLDB module which
        # fills in the default information
        ldbmessage = {"dn": user_dn,
                      "sAMAccountName": username,
                      "userPrincipalName": user_principal_name,
                      "objectClass": "user"}

        if smartcard_required:
            ldbmessage["userAccountControl"] = str(dsdb.UF_NORMAL_ACCOUNT |
                                                   dsdb.UF_SMARTCARD_REQUIRED)
            setpassword = False

        if surname is not None:
            ldbmessage["sn"] = surname

        if givenname is not None:
            ldbmessage["givenName"] = givenname

        if displayname != "":
            ldbmessage["displayName"] = displayname
            ldbmessage["name"] = displayname

        if initials is not None:
            ldbmessage["initials"] = '%s.' % initials

        if profilepath is not None:
            ldbmessage["profilePath"] = profilepath

        if scriptpath is not None:
            ldbmessage["scriptPath"] = scriptpath

        if homedrive is not None:
            ldbmessage["homeDrive"] = homedrive

        if homedirectory is not None:
            ldbmessage["homeDirectory"] = homedirectory

        if jobtitle is not None:
            ldbmessage["title"] = jobtitle

        if department is not None:
            ldbmessage["department"] = department

        if company is not None:
            ldbmessage["company"] = company

        if description is not None:
            ldbmessage["description"] = description

        if mailaddress is not None:
            ldbmessage["mail"] = mailaddress

        if internetaddress is not None:
            ldbmessage["wWWHomePage"] = internetaddress

        if telephonenumber is not None:
            ldbmessage["telephoneNumber"] = telephonenumber

        if physicaldeliveryoffice is not None:
            ldbmessage["physicalDeliveryOfficeName"] = physicaldeliveryoffice

        if sd is not None:
            ldbmessage["nTSecurityDescriptor"] = ndr_pack(sd)

        ldbmessage2 = None
        if any(map(lambda b: b is not None, (uid, uidnumber, gidnumber, gecos,
                                             loginshell, nisdomain, unixhome))):
            ldbmessage2 = ldb.Message()
            ldbmessage2.dn = ldb.Dn(self, user_dn)
            if uid is not None:
                ldbmessage2["uid"] = ldb.MessageElement(str(uid), ldb.FLAG_MOD_REPLACE, 'uid')
            if uidnumber is not None:
                ldbmessage2["uidNumber"] = ldb.MessageElement(str(uidnumber), ldb.FLAG_MOD_REPLACE, 'uidNumber')
            if gidnumber is not None:
                ldbmessage2["gidNumber"] = ldb.MessageElement(str(gidnumber), ldb.FLAG_MOD_REPLACE, 'gidNumber')
            if gecos is not None:
                ldbmessage2["gecos"] = ldb.MessageElement(str(gecos), ldb.FLAG_MOD_REPLACE, 'gecos')
            if loginshell is not None:
                ldbmessage2["loginShell"] = ldb.MessageElement(str(loginshell), ldb.FLAG_MOD_REPLACE, 'loginShell')
            if unixhome is not None:
                ldbmessage2["unixHomeDirectory"] = ldb.MessageElement(
                    str(unixhome), ldb.FLAG_MOD_REPLACE, 'unixHomeDirectory')
            if nisdomain is not None:
                ldbmessage2["msSFU30NisDomain"] = ldb.MessageElement(
                    str(nisdomain), ldb.FLAG_MOD_REPLACE, 'msSFU30NisDomain')
                ldbmessage2["msSFU30Name"] = ldb.MessageElement(
                    str(username), ldb.FLAG_MOD_REPLACE, 'msSFU30Name')
                ldbmessage2["unixUserPassword"] = ldb.MessageElement(
                    'ABCD!efgh12345$67890', ldb.FLAG_MOD_REPLACE,
                    'unixUserPassword')

        self.transaction_start()
        try:
            self.add(ldbmessage)

            with self._CleanUpOnError(self, user_dn):
                if ldbmessage2:
                    self.modify(ldbmessage2)

                # Sets the password for it
                if setpassword:
                    self.setpassword(("(distinguishedName=%s)" %
                                      ldb.binary_encode(user_dn)),
                                     password,
                                     force_password_change_at_next_login_req)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def newcontact(self,
                   fullcontactname=None,
                   ou=None,
                   surname=None,
                   givenname=None,
                   initials=None,
                   displayname=None,
                   jobtitle=None,
                   department=None,
                   company=None,
                   description=None,
                   mailaddress=None,
                   internetaddress=None,
                   telephonenumber=None,
                   mobilenumber=None,
                   physicaldeliveryoffice=None):
        """Adds a new contact with additional parameters

        :param fullcontactname: Optional full name of the new contact
        :param ou: Object container for new contact
        :param surname: Surname of the new contact
        :param givenname: First name of the new contact
        :param initials: Initials of the new contact
        :param displayname: displayName of the new contact
        :param jobtitle: Job title of the new contact
        :param department: Department of the new contact
        :param company: Company of the new contact
        :param description: Description of the new contact
        :param mailaddress: Email address of the new contact
        :param internetaddress: Home page of the new contact
        :param telephonenumber: Phone number of the new contact
        :param mobilenumber: Primary mobile number of the new contact
        :param physicaldeliveryoffice: Office location of the new contact
        """

        # Prepare the contact name like the RSAT, using the name parts.
        cn = self.fullname_from_names(given_name=givenname,
                                      initials=initials,
                                      surname=surname)

        # Use the specified fullcontactname instead of the previously prepared
        # contact name, if it is specified.
        # This is similar to the "Full name" value of the RSAT.
        if fullcontactname is not None:
            cn = fullcontactname

        if fullcontactname is None and cn == "":
            raise Exception('No name for contact specified')

        contactcontainer_dn = self.domain_dn()
        if ou:
            contactcontainer_dn = self.normalize_dn_in_domain(ou)

        contact_dn = "CN=%s,%s" % (cn, contactcontainer_dn)

        ldbmessage = {"dn": contact_dn,
                      "objectClass": "contact",
                      }

        if surname is not None:
            ldbmessage["sn"] = surname

        if givenname is not None:
            ldbmessage["givenName"] = givenname

        if displayname is not None:
            ldbmessage["displayName"] = displayname

        if initials is not None:
            ldbmessage["initials"] = '%s.' % initials

        if jobtitle is not None:
            ldbmessage["title"] = jobtitle

        if department is not None:
            ldbmessage["department"] = department

        if company is not None:
            ldbmessage["company"] = company

        if description is not None:
            ldbmessage["description"] = description

        if mailaddress is not None:
            ldbmessage["mail"] = mailaddress

        if internetaddress is not None:
            ldbmessage["wWWHomePage"] = internetaddress

        if telephonenumber is not None:
            ldbmessage["telephoneNumber"] = telephonenumber

        if mobilenumber is not None:
            ldbmessage["mobile"] = mobilenumber

        if physicaldeliveryoffice is not None:
            ldbmessage["physicalDeliveryOfficeName"] = physicaldeliveryoffice

        self.add(ldbmessage)

        return cn

    def newcomputer(self, computername, computerou=None, description=None,
                    prepare_oldjoin=False, ip_address_list=None,
                    service_principal_name_list=None):
        """Adds a new user with additional parameters

        :param computername: Name of the new computer
        :param computerou: Object container for new computer
        :param description: Description of the new computer
        :param prepare_oldjoin: Preset computer password for oldjoin mechanism
        :param ip_address_list: ip address list for DNS A or AAAA record
        :param service_principal_name_list: string list of servicePincipalName
        """

        cn = re.sub(r"\$$", "", computername)
        if cn.count('$'):
            raise Exception('Illegal computername "%s"' % computername)
        samaccountname = "%s$" % cn

        computercontainer_dn = self.get_wellknown_dn(self.get_default_basedn(),
                                              dsdb.DS_GUID_COMPUTERS_CONTAINER)
        if computerou:
            computercontainer_dn = self.normalize_dn_in_domain(computerou)

        computer_dn = "CN=%s,%s" % (cn, computercontainer_dn)

        ldbmessage = {"dn": computer_dn,
                      "sAMAccountName": samaccountname,
                      "objectClass": "computer",
                      }

        if description is not None:
            ldbmessage["description"] = description

        if service_principal_name_list:
            ldbmessage["servicePrincipalName"] = service_principal_name_list

        accountcontrol = str(dsdb.UF_WORKSTATION_TRUST_ACCOUNT |
                             dsdb.UF_ACCOUNTDISABLE)
        if prepare_oldjoin:
            accountcontrol = str(dsdb.UF_WORKSTATION_TRUST_ACCOUNT)
        ldbmessage["userAccountControl"] = accountcontrol

        if ip_address_list:
            ldbmessage['dNSHostName'] = '{}.{}'.format(
                cn, self.domain_dns_name())

        self.transaction_start()
        try:
            self.add(ldbmessage)

            if prepare_oldjoin:
                password = cn.lower()
                with self._CleanUpOnError(self, computer_dn):
                    self.setpassword(("(distinguishedName=%s)" %
                                      ldb.binary_encode(computer_dn)),
                                     password, False)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def deleteuser(self, username):
        """Deletes a user

        :param username: Name of the target user
        """

        filter = "(&(sAMAccountName=%s)(objectCategory=%s,%s))" % (ldb.binary_encode(username), "CN=Person,CN=Schema,CN=Configuration", self.domain_dn())
        self.transaction_start()
        try:
            target = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                                 expression=filter, attrs=[])
            if len(target) == 0:
                raise Exception('Unable to find user "%s"' % username)
            assert(len(target) == 1)
            self.delete(target[0].dn)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def setpassword(self, search_filter, password,
                    force_change_at_next_login=False, username=None):
        """Sets the password for a user

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        :param password: Password for the user
        :param force_change_at_next_login: Force password change
        """
        self.transaction_start()
        try:
            res = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                              expression=search_filter, attrs=[])
            if len(res) == 0:
                raise Exception('Unable to find user "%s"' % (username or search_filter))
            if len(res) > 1:
                raise Exception('Matched %u multiple users with filter "%s"' % (len(res), search_filter))
            user_dn = res[0].dn
            if not isinstance(password, str):
                pw = password.decode('utf-8')
            else:
                pw = password
            pw = ('"' + pw + '"').encode('utf-16-le')
            setpw = """
dn: %s
changetype: modify
replace: unicodePwd
unicodePwd:: %s
""" % (user_dn, base64.b64encode(pw).decode('utf-8'))

            self.modify_ldif(setpw)

            if force_change_at_next_login:
                self.force_password_change_at_next_login(
                    "(distinguishedName=" + str(user_dn) + ")")

            #  modify the userAccountControl to remove the disabled bit
            self.enable_account(search_filter)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def setexpiry(self, search_filter, expiry_seconds, no_expiry_req=False):
        """Sets the account expiry for a user

        :param search_filter: LDAP filter to find the user (eg
            sAMAccountName=name)
        :param expiry_seconds: expiry time from now in seconds
        :param no_expiry_req: if set, then don't expire password
        """
        self.transaction_start()
        try:
            res = self.search(base=self.domain_dn(), scope=ldb.SCOPE_SUBTREE,
                              expression=search_filter,
                              attrs=["userAccountControl", "accountExpires"])
            if len(res) == 0:
                raise Exception('Unable to find user "%s"' % search_filter)
            assert(len(res) == 1)
            user_dn = res[0].dn

            userAccountControl = int(res[0]["userAccountControl"][0])
            if no_expiry_req:
                userAccountControl = userAccountControl | 0x10000
                accountExpires = 0
            else:
                userAccountControl = userAccountControl & ~0x10000
                accountExpires = samba.unix2nttime(expiry_seconds + int(time.time()))

            setexp = """
dn: %s
changetype: modify
replace: userAccountControl
userAccountControl: %u
replace: accountExpires
accountExpires: %u
""" % (user_dn, userAccountControl, accountExpires)

            self.modify_ldif(setexp)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()

    def set_domain_sid(self, sid):
        """Change the domain SID used by this LDB.

        :param sid: The new domain sid to use.
        """
        dsdb._samdb_set_domain_sid(self, sid)

    def get_domain_sid(self):
        """Read the domain SID used by this LDB. """
        return dsdb._samdb_get_domain_sid(self)

    domain_sid = property(get_domain_sid, set_domain_sid,
                          doc="SID for the domain")

    def get_connecting_user_sid(self):
        """Returns the SID of the connected user."""
        msg = self.search(base="", scope=ldb.SCOPE_BASE, attrs=["tokenGroups"])[0]
        return str(ndr_unpack(security.dom_sid, msg["tokenGroups"][0]))

    connecting_user_sid = property(get_connecting_user_sid,
                                   doc="SID of the connecting user")

    def set_invocation_id(self, invocation_id):
        """Set the invocation id for this SamDB handle.

        :param invocation_id: GUID of the invocation id.
        """
        dsdb._dsdb_set_ntds_invocation_id(self, invocation_id)

    def get_invocation_id(self):
        """Get the invocation_id id"""
        return dsdb._samdb_ntds_invocation_id(self)

    invocation_id = property(get_invocation_id, set_invocation_id,
                             doc="Invocation ID GUID")

    def get_oid_from_attid(self, attid):
        return dsdb._dsdb_get_oid_from_attid(self, attid)

    def get_attid_from_lDAPDisplayName(self, ldap_display_name,
                                       is_schema_nc=False):
        """return the attribute ID for a LDAP attribute as an integer as found in DRSUAPI"""
        return dsdb._dsdb_get_attid_from_lDAPDisplayName(self,
                                                         ldap_display_name, is_schema_nc)

    def get_syntax_oid_from_lDAPDisplayName(self, ldap_display_name):
        """return the syntax OID for a LDAP attribute as a string"""
        return dsdb._dsdb_get_syntax_oid_from_lDAPDisplayName(self, ldap_display_name)

    def get_searchFlags_from_lDAPDisplayName(self, ldap_display_name):
        """return the searchFlags for a LDAP attribute as a integer"""
        return dsdb._dsdb_get_searchFlags_from_lDAPDisplayName(self, ldap_display_name)

    def get_systemFlags_from_lDAPDisplayName(self, ldap_display_name):
        """return the systemFlags for a LDAP attribute as a integer"""
        return dsdb._dsdb_get_systemFlags_from_lDAPDisplayName(self, ldap_display_name)

    def get_linkId_from_lDAPDisplayName(self, ldap_display_name):
        """return the linkID for a LDAP attribute as a integer"""
        return dsdb._dsdb_get_linkId_from_lDAPDisplayName(self, ldap_display_name)

    def get_lDAPDisplayName_by_attid(self, attid):
        """return the lDAPDisplayName from an integer DRS attribute ID"""
        return dsdb._dsdb_get_lDAPDisplayName_by_attid(self, attid)

    def get_backlink_from_lDAPDisplayName(self, ldap_display_name):
        """return the attribute name of the corresponding backlink from the name
        of a forward link attribute. If there is no backlink return None"""
        return dsdb._dsdb_get_backlink_from_lDAPDisplayName(self, ldap_display_name)

    def get_lDAPDisplayName_by_governsID_id(self, governs_idns_id):
        """return the lDAPDisplayName from an integer DRS governsID"""
        return dsdb._dsdb_get_lDAPDisplayName_by_governsID_id(self, governs_idns_id)

    def get_must_contain_from_lDAPDisplayName(self, ldap_display_name):
        """return the mandatory attributes for a LDAP class as a set of strings"""
        return dsdb._dsdb_get_must_contain_from_lDAPDisplayName(self, ldap_display_name)

    def set_ntds_settings_dn(self, ntds_settings_dn):
        """Set the NTDS Settings DN, as would be returned on the dsServiceName
        rootDSE attribute.

        This allows the DN to be set before the database fully exists

        :param ntds_settings_dn: The new DN to use
        """
        dsdb._samdb_set_ntds_settings_dn(self, ntds_settings_dn)

    def get_ntds_GUID(self):
        """Get the NTDS objectGUID"""
        return dsdb._samdb_ntds_objectGUID(self)

    def get_timestr(self):
        """Get the current time as generalized time string"""
        res = self.search(base="",
                          scope=ldb.SCOPE_BASE,
                          attrs=["currentTime"])
        return str(res[0]["currentTime"][0])

    def get_time(self):
        """Get the current time as UNIX time"""
        return ldb.string_to_time(self.get_timestr())

    def get_nttime(self):
        """Get the current time as NT time"""
        return samba.unix2nttime(self.get_time())

    def server_site_name(self):
        """Get the server site name"""
        return dsdb._samdb_server_site_name(self)

    def host_dns_name(self):
        """return the DNS name of this host"""
        res = self.search(base='', scope=ldb.SCOPE_BASE, attrs=['dNSHostName'])
        return str(res[0]['dNSHostName'][0])

    def domain_dns_name(self):
        """return the DNS name of the domain root"""
        domain_dn = self.get_default_basedn()
        return domain_dn.canonical_str().split('/')[0]

    def domain_netbios_name(self):
        """return the NetBIOS name of the domain root"""
        domain_dn = self.get_default_basedn()
        dns_name = self.domain_dns_name()
        filter = "(&(objectClass=crossRef)(nETBIOSName=*)(ncName=%s)(dnsroot=%s))" % (domain_dn, dns_name)
        partitions_dn = self.get_partitions_dn()
        res = self.search(partitions_dn,
                          scope=ldb.SCOPE_ONELEVEL,
                          expression=filter)
        try:
            netbios_domain = res[0]["nETBIOSName"][0].decode()
        except IndexError:
            return None
        return netbios_domain

    def forest_dns_name(self):
        """return the DNS name of the forest root"""
        forest_dn = self.get_root_basedn()
        return forest_dn.canonical_str().split('/')[0]

    def load_partition_usn(self, base_dn):
        return dsdb._dsdb_load_partition_usn(self, base_dn)

    def set_schema(self, schema, write_indices_and_attributes=True):
        self.set_schema_from_ldb(schema.ldb, write_indices_and_attributes=write_indices_and_attributes)

    def set_schema_from_ldb(self, ldb_conn, write_indices_and_attributes=True):
        dsdb._dsdb_set_schema_from_ldb(self, ldb_conn, write_indices_and_attributes)

    def set_schema_update_now(self):
        ldif = """
dn:
changetype: modify
add: schemaUpdateNow
schemaUpdateNow: 1
"""
        self.modify_ldif(ldif)

    def dsdb_DsReplicaAttribute(self, ldb, ldap_display_name, ldif_elements):
        """convert a list of attribute values to a DRSUAPI DsReplicaAttribute"""
        return dsdb._dsdb_DsReplicaAttribute(ldb, ldap_display_name, ldif_elements)

    def dsdb_normalise_attributes(self, ldb, ldap_display_name, ldif_elements):
        """normalise a list of attribute values"""
        return dsdb._dsdb_normalise_attributes(ldb, ldap_display_name, ldif_elements)

    def get_attribute_from_attid(self, attid):
        """ Get from an attid the associated attribute

        :param attid: The attribute id for searched attribute
        :return: The name of the attribute associated with this id
        """
        if len(self.hash_oid_name.keys()) == 0:
            self._populate_oid_attid()
        if self.get_oid_from_attid(attid) in self.hash_oid_name:
            return self.hash_oid_name[self.get_oid_from_attid(attid)]
        else:
            return None

    def _populate_oid_attid(self):
        """Populate the hash hash_oid_name.

        This hash contains the oid of the attribute as a key and
        its display name as a value
        """
        self.hash_oid_name = {}
        res = self.search(expression="objectClass=attributeSchema",
                          controls=["search_options:1:2"],
                          attrs=["attributeID",
                                 "lDAPDisplayName"])
        if len(res) > 0:
            for e in res:
                strDisplay = str(e.get("lDAPDisplayName"))
                self.hash_oid_name[str(e.get("attributeID"))] = strDisplay

    def get_attribute_replmetadata_version(self, dn, att):
        """Get the version field trom the replPropertyMetaData for
        the given field

        :param dn: The on which we want to get the version
        :param att: The name of the attribute
        :return: The value of the version field in the replPropertyMetaData
            for the given attribute. None if the attribute is not replicated
        """

        res = self.search(expression="distinguishedName=%s" % dn,
                          scope=ldb.SCOPE_SUBTREE,
                          controls=["search_options:1:2"],
                          attrs=["replPropertyMetaData"])
        if len(res) == 0:
            return None

        repl = ndr_unpack(drsblobs.replPropertyMetaDataBlob,
                          res[0]["replPropertyMetaData"][0])
        ctr = repl.ctr
        if len(self.hash_oid_name.keys()) == 0:
            self._populate_oid_attid()
        for o in ctr.array:
            # Search for Description
            att_oid = self.get_oid_from_attid(o.attid)
            if att_oid in self.hash_oid_name and\
               att.lower() == self.hash_oid_name[att_oid].lower():
                return o.version
        return None

    def set_attribute_replmetadata_version(self, dn, att, value,
                                           addifnotexist=False):
        res = self.search(expression="distinguishedName=%s" % dn,
                          scope=ldb.SCOPE_SUBTREE,
                          controls=["search_options:1:2"],
                          attrs=["replPropertyMetaData"])
        if len(res) == 0:
            return None

        repl = ndr_unpack(drsblobs.replPropertyMetaDataBlob,
                          res[0]["replPropertyMetaData"][0])
        ctr = repl.ctr
        now = samba.unix2nttime(int(time.time()))
        found = False
        if len(self.hash_oid_name.keys()) == 0:
            self._populate_oid_attid()
        for o in ctr.array:
            # Search for Description
            att_oid = self.get_oid_from_attid(o.attid)
            if att_oid in self.hash_oid_name and\
               att.lower() == self.hash_oid_name[att_oid].lower():
                found = True
                seq = self.sequence_number(ldb.SEQ_NEXT)
                o.version = value
                o.originating_change_time = now
                o.originating_invocation_id = misc.GUID(self.get_invocation_id())
                o.originating_usn = seq
                o.local_usn = seq

        if not found and addifnotexist and len(ctr.array) > 0:
            o2 = drsblobs.replPropertyMetaData1()
            o2.attid = 589914
            att_oid = self.get_oid_from_attid(o2.attid)
            seq = self.sequence_number(ldb.SEQ_NEXT)
            o2.version = value
            o2.originating_change_time = now
            o2.originating_invocation_id = misc.GUID(self.get_invocation_id())
            o2.originating_usn = seq
            o2.local_usn = seq
            found = True
            tab = ctr.array
            tab.append(o2)
            ctr.count = ctr.count + 1
            ctr.array = tab

        if found:
            replBlob = ndr_pack(repl)
            msg = ldb.Message()
            msg.dn = res[0].dn
            msg["replPropertyMetaData"] = \
                ldb.MessageElement(replBlob,
                                   ldb.FLAG_MOD_REPLACE,
                                   "replPropertyMetaData")
            self.modify(msg, ["local_oid:1.3.6.1.4.1.7165.4.3.14:0"])

    def write_prefixes_from_schema(self):
        dsdb._dsdb_write_prefixes_from_schema_to_ldb(self)

    def get_partitions_dn(self):
        return dsdb._dsdb_get_partitions_dn(self)

    def get_nc_root(self, dn):
        return dsdb._dsdb_get_nc_root(self, dn)

    def get_wellknown_dn(self, nc_root, wkguid):
        return dsdb._dsdb_get_wellknown_dn(self, nc_root, wkguid)

    def set_minPwdAge(self, value):
        if not isinstance(value, bytes):
            value = str(value).encode('utf8')
        m = ldb.Message()
        m.dn = ldb.Dn(self, self.domain_dn())
        m["minPwdAge"] = ldb.MessageElement(value, ldb.FLAG_MOD_REPLACE, "minPwdAge")
        self.modify(m)

    def get_minPwdAge(self):
        res = self.search(self.domain_dn(), scope=ldb.SCOPE_BASE, attrs=["minPwdAge"])
        if len(res) == 0:
            return None
        elif "minPwdAge" not in res[0]:
            return None
        else:
            return int(res[0]["minPwdAge"][0])

    def set_maxPwdAge(self, value):
        if not isinstance(value, bytes):
            value = str(value).encode('utf8')
        m = ldb.Message()
        m.dn = ldb.Dn(self, self.domain_dn())
        m["maxPwdAge"] = ldb.MessageElement(value, ldb.FLAG_MOD_REPLACE, "maxPwdAge")
        self.modify(m)

    def get_maxPwdAge(self):
        res = self.search(self.domain_dn(), scope=ldb.SCOPE_BASE, attrs=["maxPwdAge"])
        if len(res) == 0:
            return None
        elif "maxPwdAge" not in res[0]:
            return None
        else:
            return int(res[0]["maxPwdAge"][0])

    def set_minPwdLength(self, value):
        if not isinstance(value, bytes):
            value = str(value).encode('utf8')
        m = ldb.Message()
        m.dn = ldb.Dn(self, self.domain_dn())
        m["minPwdLength"] = ldb.MessageElement(value, ldb.FLAG_MOD_REPLACE, "minPwdLength")
        self.modify(m)

    def get_minPwdLength(self):
        res = self.search(self.domain_dn(), scope=ldb.SCOPE_BASE, attrs=["minPwdLength"])
        if len(res) == 0:
            return None
        elif "minPwdLength" not in res[0]:
            return None
        else:
            return int(res[0]["minPwdLength"][0])

    def set_pwdProperties(self, value):
        if not isinstance(value, bytes):
            value = str(value).encode('utf8')
        m = ldb.Message()
        m.dn = ldb.Dn(self, self.domain_dn())
        m["pwdProperties"] = ldb.MessageElement(value, ldb.FLAG_MOD_REPLACE, "pwdProperties")
        self.modify(m)

    def get_pwdProperties(self):
        res = self.search(self.domain_dn(), scope=ldb.SCOPE_BASE, attrs=["pwdProperties"])
        if len(res) == 0:
            return None
        elif "pwdProperties" not in res[0]:
            return None
        else:
            return int(res[0]["pwdProperties"][0])

    def set_dsheuristics(self, dsheuristics):
        m = ldb.Message()
        m.dn = ldb.Dn(self, "CN=Directory Service,CN=Windows NT,CN=Services,%s"
                      % self.get_config_basedn().get_linearized())
        if dsheuristics is not None:
            m["dSHeuristics"] = \
                ldb.MessageElement(dsheuristics,
                                   ldb.FLAG_MOD_REPLACE,
                                   "dSHeuristics")
        else:
            m["dSHeuristics"] = \
                ldb.MessageElement([], ldb.FLAG_MOD_DELETE,
                                   "dSHeuristics")
        self.modify(m)

    def get_dsheuristics(self):
        res = self.search("CN=Directory Service,CN=Windows NT,CN=Services,%s"
                          % self.get_config_basedn().get_linearized(),
                          scope=ldb.SCOPE_BASE, attrs=["dSHeuristics"])
        if len(res) == 0:
            dsheuristics = None
        elif "dSHeuristics" in res[0]:
            dsheuristics = res[0]["dSHeuristics"][0]
        else:
            dsheuristics = None

        return dsheuristics

    def create_ou(self, ou_dn, description=None, name=None, sd=None):
        """Creates an organizationalUnit object
        :param ou_dn: dn of the new object
        :param description: description attribute
        :param name: name attribute
        :param sd: security descriptor of the object, can be
        an SDDL string or security.descriptor type
        """
        m = {"dn": ou_dn,
             "objectClass": "organizationalUnit"}

        if description:
            m["description"] = description
        if name:
            m["name"] = name

        if sd:
            m["nTSecurityDescriptor"] = ndr_pack(sd)
        self.add(m)

    def sequence_number(self, seq_type):
        """Returns the value of the sequence number according to the requested type
        :param seq_type: type of sequence number
         """
        self.transaction_start()
        try:
            seq = super().sequence_number(seq_type)
        except:
            self.transaction_cancel()
            raise
        else:
            self.transaction_commit()
        return seq

    def get_dsServiceName(self):
        """get the NTDS DN from the rootDSE"""
        res = self.search(base="", scope=ldb.SCOPE_BASE, attrs=["dsServiceName"])
        return str(res[0]["dsServiceName"][0])

    def get_serverName(self):
        """get the server DN from the rootDSE"""
        res = self.search(base="", scope=ldb.SCOPE_BASE, attrs=["serverName"])
        return str(res[0]["serverName"][0])

    def dns_lookup(self, dns_name, dns_partition=None):
        """Do a DNS lookup in the database, returns the NDR database structures"""
        if dns_partition is None:
            return dsdb_dns.lookup(self, dns_name)
        else:
            return dsdb_dns.lookup(self, dns_name,
                                   dns_partition=dns_partition)

    def dns_extract(self, el):
        """Return the NDR database structures from a dnsRecord element"""
        return dsdb_dns.extract(self, el)

    def dns_replace(self, dns_name, new_records):
        """Do a DNS modification on the database, sets the NDR database
        structures on a DNS name
        """
        return dsdb_dns.replace(self, dns_name, new_records)

    def dns_replace_by_dn(self, dn, new_records):
        """Do a DNS modification on the database, sets the NDR database
        structures on a LDB DN

        This routine is important because if the last record on the DN
        is removed, this routine will put a tombstone in the record.
        """
        return dsdb_dns.replace_by_dn(self, dn, new_records)

    def garbage_collect_tombstones(self, dn, current_time,
                                   tombstone_lifetime=None):
        """garbage_collect_tombstones(lp, samdb, [dn], current_time, tombstone_lifetime)
        -> (num_objects_expunged, num_links_expunged)"""

        if not is_ad_dc_built():
            raise SamDBError('Cannot garbage collect tombstones: '
                'AD DC was not built')

        if tombstone_lifetime is None:
            return dsdb._dsdb_garbage_collect_tombstones(self, dn,
                                                         current_time)
        else:
            return dsdb._dsdb_garbage_collect_tombstones(self, dn,
                                                         current_time,
                                                         tombstone_lifetime)

    def create_own_rid_set(self):
        """create a RID set for this DSA"""
        return dsdb._dsdb_create_own_rid_set(self)

    def allocate_rid(self):
        """return a new RID from the RID Pool on this DSA"""
        return dsdb._dsdb_allocate_rid(self)

    def next_free_rid(self):
        """return the next free RID from the RID Pool on this DSA.

        :note: This function is not intended for general use, and care must be
            taken if it is used to generate objectSIDs. The returned RID is not
            formally reserved for use, creating the possibility of duplicate
            objectSIDs.
        """
        rid, _ = self.free_rid_bounds()
        return rid

    def free_rid_bounds(self):
        """return the low and high bounds (inclusive) of RIDs that are
            available for use in this DSA's current RID pool.

        :note: This function is not intended for general use, and care must be
            taken if it is used to generate objectSIDs. The returned range of
            RIDs is not formally reserved for use, creating the possibility of
            duplicate objectSIDs.
        """
        # Get DN of this server's RID Set
        server_name_dn = ldb.Dn(self, self.get_serverName())
        res = self.search(base=server_name_dn,
                          scope=ldb.SCOPE_BASE,
                          attrs=["serverReference"])
        try:
            server_ref = res[0]["serverReference"]
        except KeyError:
            raise ldb.LdbError(
                ldb.ERR_NO_SUCH_ATTRIBUTE,
                "No RID Set DN - "
                "Cannot find attribute serverReference of %s "
                "to calculate reference dn" % server_name_dn) from None
        server_ref_dn = ldb.Dn(self, server_ref[0].decode("utf-8"))

        res = self.search(base=server_ref_dn,
                          scope=ldb.SCOPE_BASE,
                          attrs=["rIDSetReferences"])
        try:
            rid_set_refs = res[0]["rIDSetReferences"]
        except KeyError:
            raise ldb.LdbError(
                ldb.ERR_NO_SUCH_ATTRIBUTE,
                "No RID Set DN - "
                "Cannot find attribute rIDSetReferences of %s "
                "to calculate reference dn" % server_ref_dn) from None
        rid_set_dn = ldb.Dn(self, rid_set_refs[0].decode("utf-8"))

        # Get the alloc pools and next RID of this RID Set
        res = self.search(base=rid_set_dn,
                          scope=ldb.SCOPE_BASE,
                          attrs=["rIDAllocationPool",
                                 "rIDPreviousAllocationPool",
                                 "rIDNextRID"])

        uint32_max = 2**32 - 1
        uint64_max = 2**64 - 1

        try:
            alloc_pool = int(res[0]["rIDAllocationPool"][0])
        except KeyError:
            alloc_pool = uint64_max
        if alloc_pool == uint64_max:
            raise ldb.LdbError(ldb.ERR_OPERATIONS_ERROR,
                               "Bad RID Set %s" % rid_set_dn)

        try:
            prev_pool = int(res[0]["rIDPreviousAllocationPool"][0])
        except KeyError:
            prev_pool = uint64_max
        try:
            next_rid = int(res[0]["rIDNextRID"][0])
        except KeyError:
            next_rid = uint32_max

        # If we never used a pool, set up our first pool
        if prev_pool == uint64_max or next_rid == uint32_max:
            prev_pool = alloc_pool
            next_rid = prev_pool & uint32_max
        else:
            next_rid += 1

        # Now check if our current pool is still usable
        prev_pool_lo = prev_pool & uint32_max
        prev_pool_hi = prev_pool >> 32
        if next_rid > prev_pool_hi:
            # We need a new pool, check if we already have a new one
            # Otherwise we return an error code.
            if alloc_pool == prev_pool:
                raise ldb.LdbError(ldb.ERR_OPERATIONS_ERROR,
                                   "RID pools out of RIDs")

            # Now use the new pool
            prev_pool = alloc_pool
            prev_pool_lo = prev_pool & uint32_max
            prev_pool_hi = prev_pool >> 32
            next_rid = prev_pool_lo

        if next_rid < prev_pool_lo or next_rid > prev_pool_hi:
            raise ldb.LdbError(ldb.ERR_OPERATIONS_ERROR,
                               "Bad RID chosen %d from range %d-%d" %
                               (next_rid, prev_pool_lo, prev_pool_hi))

        return next_rid, prev_pool_hi

    def normalize_dn_in_domain(self, dn):
        """return a new DN expanded by adding the domain DN

        If the dn is already a child of the domain DN, just
        return it as-is.

        :param dn: relative dn
        """
        domain_dn = ldb.Dn(self, self.domain_dn())

        if isinstance(dn, ldb.Dn):
            dn = str(dn)

        full_dn = ldb.Dn(self, dn)
        if not full_dn.is_child_of(domain_dn):
            full_dn.add_base(domain_dn)
        return full_dn

    def new_gkdi_root_key(self, *args, **kwargs):
        """ """
        dn = dsdb._dsdb_create_gkdi_root_key(self, *args, **kwargs)
        return dn

    def get_admin_sid(self):
        res = self.search(
            base="", expression="", scope=ldb.SCOPE_BASE, attrs=["tokenGroups"])

        return self.schema_format_value(
            "tokenGroups", res[0]["tokenGroups"][0]).decode("utf8")


class dsdb_Dn(object):
    """a class for binary DN"""

    def __init__(self, samdb, dnstring, syntax_oid=None):
        """create a dsdb_Dn"""
        if syntax_oid is None:
            # auto-detect based on string
            if dnstring.startswith("B:"):
                syntax_oid = dsdb.DSDB_SYNTAX_BINARY_DN
            elif dnstring.startswith("S:"):
                syntax_oid = dsdb.DSDB_SYNTAX_STRING_DN
            else:
                syntax_oid = dsdb.DSDB_SYNTAX_OR_NAME
        if syntax_oid in [dsdb.DSDB_SYNTAX_BINARY_DN, dsdb.DSDB_SYNTAX_STRING_DN]:
            # it is a binary DN
            colons = dnstring.split(':')
            if len(colons) < 4:
                raise RuntimeError("Invalid DN %s" % dnstring)
            prefix_len = 4 + len(colons[1]) + int(colons[1])
            self.prefix = dnstring[0:prefix_len]
            self.binary = self.prefix[3 + len(colons[1]):-1]
            self.dnstring = dnstring[prefix_len:]
        else:
            self.dnstring = dnstring
            self.prefix = ''
            self.binary = ''
        self.dn = ldb.Dn(samdb, self.dnstring)

    def __str__(self):
        return self.prefix + str(self.dn.extended_str(mode=1))

    def __cmp__(self, other):
        """ compare dsdb_Dn values similar to parsed_dn_compare()"""
        dn1 = self
        dn2 = other
        guid1 = dn1.dn.get_extended_component("GUID")
        guid2 = dn2.dn.get_extended_component("GUID")

        v = cmp(guid1, guid2)
        if v != 0:
            return v
        v = cmp(dn1.binary, dn2.binary)
        return v

    # In Python3, __cmp__ is replaced by these 6 methods
    def __eq__(self, other):
        return self.__cmp__(other) == 0

    def __ne__(self, other):
        return self.__cmp__(other) != 0

    def __lt__(self, other):
        return self.__cmp__(other) < 0

    def __le__(self, other):
        return self.__cmp__(other) <= 0

    def __gt__(self, other):
        return self.__cmp__(other) > 0

    def __ge__(self, other):
        return self.__cmp__(other) >= 0

    def get_binary_integer(self):
        """return binary part of a dsdb_Dn as an integer, or None"""
        if self.prefix == '':
            return None
        return int(self.binary, 16)

    def get_bytes(self):
        """return binary as a byte string"""
        return binascii.unhexlify(self.binary)
