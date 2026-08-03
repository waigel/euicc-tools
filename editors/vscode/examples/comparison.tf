# The same profile as minimal.vn, written as HCL, to put the two side by side.
#
# This is not a format euicc reads and nothing here is used. It is here so the
# colours can be compared: open it next to minimal.vn with the HashiCorp
# Terraform extension installed and look at what each layer decides.
#
# What HCL gives a reader that value notation does not:
#
#   header "profile" {        a block type and a label, two colours
#   major_version = 2         an equals sign, so a name is always a name
#   true / false              a keyword, coloured as one
#
# What value notation gives that HCL does not:
#
#   major-version 2           no separator at all, so the grammar has to guess
#   header : { … }            a colon means a CHOICE alternative, not a value
#   '89000123456789012341'H   a literal that is neither text nor a plain number
#
# The Terraform extension declares its own semantic token types with a standard
# superType and pins each to a TextMate scope in its package.json:
#
#   hcl-attrName  -> property   -> variable.other.property
#   hcl-blockType -> type       -> entity.name.type
#   hcl-blockLabel -> enumMember -> variable.other.enummember
#
# This extension does the same, with asn1Member, asn1Alternative, asn1Value and
# asn1Type, pinned to the scopes its own grammar already uses.

locals {
  # A profile header. Clause 8.2.1 of SAIP 3.4.1.
  profile_header = {
    major_version = 2
    minor_version = 3
    profile_type  = "example"

    # An ICCID is ten bytes of ITU-T E.118, and HCL has no literal for that.
    # Value notation writes '89000123456789012341'H.
    iccid = "89000123456789012341"

    mandatory_services = {
      usim     = true
      javacard = false
    }

    mandatory_gfstelist = [
      "2.23.143.1.2.1", # the MF template
      "2.23.143.1.2.4", # ADF USIM
    ]
  }

  # The master file, from the template of Annex A 9.2.
  master_file = {
    header = {
      mandated       = true
      identification = 1
    }
    template_id = "2.23.143.1.2.1"

    ef_dir = {
      file_descriptor                = "4221" # linear fixed, so four bytes
      file_id                        = "2F00"
      security_attributes_referenced = "2F0601"
      ef_file_size                   = "0A"
    }
  }
}

variable "profile_type" {
  description = "Free text, 1 to 100 characters. UTF8String in the schema."
  type        = string
  default     = "example"

  validation {
    condition     = length(var.profile_type) > 0 && length(var.profile_type) <= 100
    error_message = "SIZE (1..100), which euicc reports as a constraint failure."
  }
}

output "iccid" {
  description = "What the header carries. EF_ICCID holds the same digits with the nibbles of each byte swapped, which is why a profile has two."
  value       = local.profile_header.iccid
}
