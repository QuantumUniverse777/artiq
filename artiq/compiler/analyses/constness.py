"""
:class:`Constness` checks that no attribute marked
as constant is ever set.
"""

from pythonparser import algorithm, diagnostic
from .. import types

class Constness(algorithm.Visitor):
    def __init__(self, engine):
        self.engine = engine
        self.in_assign = False

    def visit_Assign(self, node):
        self.visit(node.value)
        self.in_assign = True
        self.visit(node.targets)
        self.in_assign = False

    def visit_SubscriptT(self, node):
        old_in_assign, self.in_assign = self.in_assign, False
        self.visit(node.value)
        self.visit(node.slice)
        self.in_assign = old_in_assign

    def visit_AttributeT(self, node):
        old_in_assign, self.in_assign = self.in_assign, False
        self.visit(node.value)
        self.in_assign = old_in_assign

        if self.in_assign:
            typ = node.value.type.find()
            if types.is_instance(typ) and node.attr in typ.constant_attributes:
                diag = diagnostic.Diagnostic("error",
                    "cannot assign to constant attribute '{attr}' of class '{class}'",
                    {"attr": node.attr, "class": typ.name},
                    node.loc)
                self.engine.process(diag)
                return
